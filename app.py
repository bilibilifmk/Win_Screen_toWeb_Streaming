import os
import time
import threading
from typing import Optional

import cv2
import numpy as np
from flask import Flask, Response, render_template, request
from mss import mss


app = Flask(__name__)


class ScreenEncoder:
    def __init__(self, fps: int = 12, jpeg_quality: int = 75, monitor_index: int = 1):
        self.fps = max(1, fps)
        self.jpeg_quality = max(20, min(95, jpeg_quality))
        self.monitor_index = max(1, monitor_index)

        self._latest_jpeg: Optional[bytes] = None
        self._lock = threading.Lock()
        self._running = False
        self._thread: Optional[threading.Thread] = None

    def start(self) -> None:
        if self._running:
            return
        self._running = True
        self._thread = threading.Thread(target=self._capture_loop, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._running = False
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=1)

    def _capture_loop(self) -> None:
        frame_interval = 1.0 / self.fps
        with mss() as sct:
            monitors = sct.monitors
            if self.monitor_index >= len(monitors):
                self.monitor_index = 1

            monitor = monitors[self.monitor_index]

            while self._running:
                start = time.time()

                raw = sct.grab(monitor)
                frame = np.array(raw)
                frame = cv2.cvtColor(frame, cv2.COLOR_BGRA2BGR)

                ok, encoded = cv2.imencode(
                    ".jpg",
                    frame,
                    [int(cv2.IMWRITE_JPEG_QUALITY), self.jpeg_quality],
                )
                if ok:
                    with self._lock:
                        self._latest_jpeg = encoded.tobytes()

                elapsed = time.time() - start
                sleep_time = frame_interval - elapsed
                if sleep_time > 0:
                    time.sleep(sleep_time)

    def get_frame(self) -> Optional[bytes]:
        with self._lock:
            return self._latest_jpeg


FPS = int(os.getenv("FPS", "12"))
JPEG_QUALITY = int(os.getenv("JPEG_QUALITY", "75"))
MONITOR_INDEX = int(os.getenv("MONITOR_INDEX", "1"))

encoder = ScreenEncoder(fps=FPS, jpeg_quality=JPEG_QUALITY, monitor_index=MONITOR_INDEX)
encoder.start()


@app.route("/")
def index():
    return render_template(
        "index.html",
        fps=FPS,
        jpeg_quality=JPEG_QUALITY,
        monitor_index=MONITOR_INDEX,
    )


@app.route("/stream.mjpg")
def stream_mjpg():
    fps = int(request.args.get("fps", FPS))
    boundary = "frame"

    def generate():
        frame_interval = 1.0 / max(1, fps)
        while True:
            frame = encoder.get_frame()
            if frame is None:
                time.sleep(0.01)
                continue

            yield (
                b"--" + boundary.encode() + b"\r\n"
                b"Content-Type: image/jpeg\r\n\r\n" + frame + b"\r\n"
            )
            time.sleep(frame_interval)

    return Response(
        generate(),
        mimetype=f"multipart/x-mixed-replace; boundary={boundary}",
    )


if __name__ == "__main__":
    host = os.getenv("HOST", "0.0.0.0")
    port = int(os.getenv("PORT", "8000"))
    app.run(host=host, port=port, threaded=True)
