package main

import (
	"bytes"
	"fmt"
	"image/jpeg"
	"log"
	"net/http"
	"os"
	"strconv"
	"sync"
	"time"

	"github.com/kbinani/screenshot"
)

type Encoder struct {
	fps          int
	jpegQuality  int
	displayIndex int

	mu     sync.RWMutex
	frame  []byte
	run    bool
}

func NewEncoder(fps, quality, displayIndex int) *Encoder {
	if fps < 1 {
		fps = 1
	}
	if quality < 20 {
		quality = 20
	}
	if quality > 95 {
		quality = 95
	}
	if displayIndex < 0 {
		displayIndex = 0
	}

	return &Encoder{
		fps:          fps,
		jpegQuality:  quality,
		displayIndex: displayIndex,
		run:          true,
	}
}

func (e *Encoder) Start() {
	go func() {
		interval := time.Second / time.Duration(e.fps)

		for e.run {
			start := time.Now()

			n := screenshot.NumActiveDisplays()
			if n == 0 {
				time.Sleep(500 * time.Millisecond)
				continue
			}

			idx := e.displayIndex
			if idx >= n {
				idx = 0
			}

			img, err := screenshot.CaptureDisplay(idx)
			if err != nil {
				log.Printf("capture failed: %v", err)
				time.Sleep(150 * time.Millisecond)
				continue
			}

			var buf bytes.Buffer
			err = jpeg.Encode(&buf, img, &jpeg.Options{Quality: e.jpegQuality})
			if err == nil {
				e.mu.Lock()
				e.frame = buf.Bytes()
				e.mu.Unlock()
			}

			elapsed := time.Since(start)
			if elapsed < interval {
				time.Sleep(interval - elapsed)
			}
		}
	}()
}

func (e *Encoder) Stop() {
	e.run = false
}

func (e *Encoder) Frame() []byte {
	e.mu.RLock()
	defer e.mu.RUnlock()
	if len(e.frame) == 0 {
		return nil
	}
	out := make([]byte, len(e.frame))
	copy(out, e.frame)
	return out
}

func getEnvInt(name string, def int) int {
	v := os.Getenv(name)
	if v == "" {
		return def
	}
	n, err := strconv.Atoi(v)
	if err != nil {
		return def
	}
	return n
}

func main() {
	fps := getEnvInt("FPS", 12)
	quality := getEnvInt("JPEG_QUALITY", 75)
	displayIndex := getEnvInt("DISPLAY_INDEX", 0)
	port := getEnvInt("PORT", 8000)
	host := os.Getenv("HOST")
	if host == "" {
		host = "0.0.0.0"
	}

	encoder := NewEncoder(fps, quality, displayIndex)
	encoder.Start()
	defer encoder.Stop()

	html := fmt.Sprintf(`<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width,initial-scale=1" />
  <title>Windows 屏幕直播</title>
  <style>
    body{margin:0;background:#101010;color:#f4f4f4;font-family:Arial,sans-serif}
    .top{padding:10px 14px;background:#1d1d1d;border-bottom:1px solid #333;display:flex;gap:10px;flex-wrap:wrap}
    .pill{background:#2a2a2a;border-radius:999px;padding:4px 10px;font-size:13px}
    .box{height:calc(100vh - 52px);display:flex;align-items:center;justify-content:center;overflow:hidden}
    img{max-width:100%%;max-height:100%%;object-fit:contain;border:1px solid #2b2b2b;background:#000}
  </style>
</head>
<body>
  <div class="top">
    <span class="pill">编码: MJPEG(JPEG)</span>
    <span class="pill">FPS: %d</span>
    <span class="pill">质量: %d</span>
    <span class="pill">显示器: %d</span>
  </div>
  <div class="box"><img src="/stream.mjpg" alt="stream"/></div>
</body>
</html>`, fps, quality, displayIndex)

	http.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		_, _ = w.Write([]byte(html))
	})

	http.HandleFunc("/stream.mjpg", func(w http.ResponseWriter, r *http.Request) {
		boundary := "frame"
		w.Header().Set("Content-Type", "multipart/x-mixed-replace; boundary="+boundary)
		w.Header().Set("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0")

		flusher, ok := w.(http.Flusher)
		if !ok {
			http.Error(w, "stream unsupported", http.StatusInternalServerError)
			return
		}

		ticker := time.NewTicker(time.Second / time.Duration(fps))
		defer ticker.Stop()

		for {
			select {
			case <-r.Context().Done():
				return
			case <-ticker.C:
				frame := encoder.Frame()
				if frame == nil {
					continue
				}
				_, err := w.Write([]byte("--" + boundary + "\r\n"))
				if err != nil {
					return
				}
				_, err = w.Write([]byte("Content-Type: image/jpeg\r\n\r\n"))
				if err != nil {
					return
				}
				_, err = w.Write(frame)
				if err != nil {
					return
				}
				_, err = w.Write([]byte("\r\n"))
				if err != nil {
					return
				}
				flusher.Flush()
			}
		}
	})

	addr := fmt.Sprintf("%s:%d", host, port)
	log.Printf("Screen web server started at http://%s", addr)
	log.Fatal(http.ListenAndServe(addr, nil))
}
