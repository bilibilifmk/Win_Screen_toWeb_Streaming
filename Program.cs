using System.Drawing;
using System.Drawing.Imaging;
using System.Text;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Http.Features;
using Microsoft.Extensions.Hosting;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace obs;

internal static class Program
{
    private static Mutex? _singleInstanceMutex;

    [STAThread]
    private static void Main(string[] args)
    {
        _singleInstanceMutex = new Mutex(initiallyOwned: true, name: @"Local\obs_single_instance", createdNew: out var createdNew);
        if (!createdNew)
        {
            return;
        }

        try
        {
            var fps = GetEnvInt("FPS", 12, 1, 60);
            var jpegQuality = GetEnvInt("JPEG_QUALITY", 75, 20, 95);
            var displayIndex = GetEnvInt("DISPLAY_INDEX", 0, 0, 16);
            var host = Environment.GetEnvironmentVariable("HOST") ?? "0.0.0.0";
            var port = GetEnvInt("PORT", 8000, 1, 65535);

            var builder = WebApplication.CreateBuilder(args);
            builder.WebHost.UseUrls($"http://{host}:{port}");
            var app = builder.Build();

            var encoder = new ScreenEncoder(fps, jpegQuality, displayIndex);
            encoder.Start();

                        var html = $$"""
<!doctype html>
<html lang="zh-CN">
<head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width,initial-scale=1" />
    <title>OBS 控制面板</title>
    <style>
        body{margin:0;background:#101010;color:#f4f4f4;font-family:Arial,sans-serif}
        .top{padding:10px 14px;background:#1d1d1d;border-bottom:1px solid #333;display:flex;gap:10px;flex-wrap:wrap;align-items:center}
        .pill{background:#2a2a2a;border-radius:999px;padding:4px 10px;font-size:13px}
        .ctrl{display:flex;gap:8px;align-items:center;background:#2a2a2a;border-radius:10px;padding:6px 10px}
        .ctrl label{font-size:13px;color:#ddd}
        .ctrl select,.ctrl input{background:#111;color:#f4f4f4;border:1px solid #444;border-radius:6px;padding:4px 6px}
        .ctrl button{background:#0a84ff;color:#fff;border:none;border-radius:6px;padding:6px 10px;cursor:pointer}
        .box{height:calc(100vh - 52px);display:flex;align-items:center;justify-content:center;overflow:hidden}
        img{max-width:100%;max-height:100%;object-fit:contain;border:1px solid #2b2b2b;background:#000}
        #msg{font-size:12px;color:#9ad07c}
    </style>
</head>
<body>
    <div class="top">
        <span class="pill" id="codecLabel">编码: MJPEG(JPEG)</span>
        <span class="pill">FPS: {{fps}}</span>
        <span class="pill" id="qualityLabel">质量: {{jpegQuality}}</span>
        <span class="pill">显示器: {{displayIndex}}</span>
        <div class="ctrl">
            <label>格式</label>
            <select id="codec">
                <option value="jpeg">JPEG</option>
                <option value="png">PNG</option>
            </select>
            <label>质量</label>
            <input id="quality" type="range" min="20" max="95" value="{{jpegQuality}}" />
            <span id="qualityValue">{{jpegQuality}}</span>
            <button id="applyBtn" type="button">应用</button>
        </div>
        <span id="msg"></span>
    </div>
    <div class="box"><img src="/stream.mjpg" alt="stream"/></div>
    <div style="position:fixed;bottom:6px;right:12px;font-size:11px;color:#555">by 小可爱</div>
    <script>
        const codecEl = document.getElementById('codec');
        const qualityEl = document.getElementById('quality');
        const qualityValueEl = document.getElementById('qualityValue');
        const msgEl = document.getElementById('msg');
        const codecLabelEl = document.getElementById('codecLabel');
        const qualityLabelEl = document.getElementById('qualityLabel');

        qualityEl.addEventListener('input', () => {
            qualityValueEl.textContent = qualityEl.value;
        });

        function syncLabels(codec, quality) {
            codecLabelEl.textContent = '编码: MJPEG(' + codec.toUpperCase() + ')';
            qualityLabelEl.textContent = '质量: ' + quality;
        }

        function syncQualityEnabled(codec) {
            const enabled = codec === 'jpeg';
            qualityEl.disabled = !enabled;
            qualityValueEl.style.opacity = enabled ? '1' : '0.5';
        }

        codecEl.addEventListener('change', () => {
            syncQualityEnabled(codecEl.value);
        });

        async function loadSettings() {
            const res = await fetch('/api/settings');
            const data = await res.json();
            codecEl.value = data.codec;
            qualityEl.value = data.jpegQuality;
            qualityValueEl.textContent = data.jpegQuality;
            syncLabels(data.codec, data.jpegQuality);
            syncQualityEnabled(data.codec);
        }

        document.getElementById('applyBtn').addEventListener('click', async () => {
            msgEl.textContent = '';
            const body = { codec: codecEl.value, jpegQuality: Number(qualityEl.value) };

            const res = await fetch('/api/settings', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(body)
            });

            if (!res.ok) {
                msgEl.style.color = '#ff7777';
                msgEl.textContent = '设置失败';
                return;
            }

            const data = await res.json();
            syncLabels(data.codec, data.jpegQuality);
            syncQualityEnabled(data.codec);
            msgEl.style.color = '#9ad07c';
            msgEl.textContent = '已应用';
            setTimeout(() => msgEl.textContent = '', 1200);
        });

        loadSettings();
    </script>
</body>
</html>
""";

                        app.MapGet("/", () => Results.Redirect("/view", permanent: false));
                        app.MapGet("/view", () => Results.Content(html, "text/html; charset=utf-8"));
            app.MapGet("/healthz", () => Results.Ok(new { ok = true }));
                        app.MapGet("/api/settings", () => Results.Json(encoder.GetSettings()));
                        app.MapPost("/api/settings", (UpdateSettingsRequest req) =>
                        {
                                if (!encoder.UpdateSettings(req.Codec, req.JpegQuality))
                                {
                                        return Results.BadRequest(new { message = "invalid settings" });
                                }
                                return Results.Json(encoder.GetSettings());
                        });

            app.MapGet("/stream.mjpg", async context =>
            {
                context.Features.Get<IHttpResponseBodyFeature>()?.DisableBuffering();
                context.Response.Headers.CacheControl = "no-store, no-cache, must-revalidate, max-age=0";
                context.Response.ContentType = "multipart/x-mixed-replace; boundary=frame";

                var ct = context.RequestAborted;
                var interval = TimeSpan.FromMilliseconds(1000.0 / fps);

                while (!ct.IsCancellationRequested)
                {
                    var frame = encoder.GetLatestFrame();
                    if (frame is null)
                    {
                        await Task.Delay(10, ct);
                        continue;
                    }

                    await context.Response.BodyWriter.WriteAsync("--frame\r\n"u8.ToArray(), ct);
                    await context.Response.BodyWriter.WriteAsync(Encoding.ASCII.GetBytes($"Content-Type: {frame.MimeType}\r\n\r\n"), ct);
                    await context.Response.BodyWriter.WriteAsync(frame.Bytes, ct);
                    await context.Response.BodyWriter.WriteAsync("\r\n"u8.ToArray(), ct);
                    await context.Response.Body.FlushAsync(ct);

                    await Task.Delay(interval, ct);
                }
            });

            var appCts = new CancellationTokenSource();
            try
            {
                app.StartAsync(appCts.Token).GetAwaiter().GetResult();
            }
            catch
            {
                encoder.Stop();
                return;
            }

            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);

            var trayContext = new TrayHostContext(host, port, async () =>
            {
                encoder.Stop();
                appCts.Cancel();
                try { await app.StopAsync(); } catch { }
            });

            Application.Run(trayContext);

            encoder.Stop();
            appCts.Cancel();
            try { app.StopAsync().GetAwaiter().GetResult(); } catch { }
        }
        finally
        {
            if (_singleInstanceMutex is not null)
            {
                try { _singleInstanceMutex.ReleaseMutex(); } catch { }
                _singleInstanceMutex.Dispose();
            }
        }
    }

    private static int GetEnvInt(string name, int defaultValue, int min, int max)
    {
        var raw = Environment.GetEnvironmentVariable(name);
        if (!int.TryParse(raw, out var value))
        {
            value = defaultValue;
        }

        if (value < min) value = min;
        if (value > max) value = max;
        return value;
    }
}

sealed class TrayHostContext : ApplicationContext
{
    private readonly Func<Task> _onExit;
    private readonly ContextMenuStrip _menu;
    private readonly NotifyIcon _notifyIcon;
    private readonly Icon _icon;
    private int _isExiting;

    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    private static extern bool DestroyIcon(IntPtr handle);

    public TrayHostContext(string host, int port, Func<Task> onExit)
    {
        _onExit = onExit;

        var displayHost = (host == "0.0.0.0") ? GetLocalIP() : host;
        var addrText = $"{displayHost}:{port}";

        _menu = new ContextMenuStrip();
        var addrItem = _menu.Items.Add(addrText);
        addrItem.Enabled = false;
        _menu.Items.Add(new ToolStripSeparator());
        _menu.Items.Add("关于", null, (_, _) =>
        {
            MessageBox.Show(
                "There are shortcuts to learning\n\n" +
                "轻量级串流工具\n" +
                $"监听地址: {addrText}\n\n" +
                "by 小可爱",
                "关于",
                MessageBoxButtons.OK,
                MessageBoxIcon.Information);
        });
        _menu.Items.Add("退出", null, async (_, _) => await ExitAsync());

        _icon = CreateTransparentTrayIcon();
        _notifyIcon = new NotifyIcon
        {
            Icon = _icon,
            ContextMenuStrip = _menu,
            Visible = true,
            Text = "obs"
        };
    }

    private async Task ExitAsync()
    {
        if (Interlocked.Exchange(ref _isExiting, 1) == 1) return;

        try { await _onExit(); } catch { }
        _notifyIcon.Visible = false;
        ExitThread();
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _notifyIcon.Visible = false;
            _notifyIcon.Dispose();
            _menu.Dispose();
            _icon.Dispose();
        }
        base.Dispose(disposing);
    }

    private static string GetLocalIP()
    {
        try
        {
            using var socket = new System.Net.Sockets.Socket(
                System.Net.Sockets.AddressFamily.InterNetwork,
                System.Net.Sockets.SocketType.Dgram, 0);
            socket.Connect("8.8.8.8", 65530);
            if (socket.LocalEndPoint is System.Net.IPEndPoint ep)
                return ep.Address.ToString();
        }
        catch { }
        return "127.0.0.1";
    }

    private static Icon CreateTransparentTrayIcon()
    {
        using var bmp = new Bitmap(16, 16, PixelFormat.Format32bppArgb);
        bmp.SetPixel(8, 8, Color.FromArgb(16, 255, 255, 255));
        var handle = bmp.GetHicon();
        try
        {
            var icon = (Icon)Icon.FromHandle(handle).Clone();
            return icon;
        }
        finally
        {
            DestroyIcon(handle);
        }
    }
}

sealed class ScreenEncoder
{
    private readonly int _fps;
    private readonly int _displayIndex;
    private readonly object _lock = new();

    private EncodedFrame? _latestFrame;
    private int _jpegQuality;
    private string _codec;
    private CancellationTokenSource? _cts;
    private Task? _worker;

    public ScreenEncoder(int fps, int jpegQuality, int displayIndex)
    {
        _fps = fps;
        _jpegQuality = jpegQuality;
        _displayIndex = displayIndex;
        _codec = "jpeg";
    }

    public void Start()
    {
        _cts = new CancellationTokenSource();
        _worker = Task.Run(() => Loop(_cts.Token));
    }

    public void Stop()
    {
        if (_cts is null) return;
        _cts.Cancel();
        try { _worker?.Wait(1000); } catch { }
        _cts.Dispose();
    }

    public EncodedFrame? GetLatestFrame()
    {
        lock (_lock)
        {
            if (_latestFrame is null) return null;
            var copy = new byte[_latestFrame.Bytes.Length];
            Buffer.BlockCopy(_latestFrame.Bytes, 0, copy, 0, _latestFrame.Bytes.Length);
            return new EncodedFrame(copy, _latestFrame.MimeType);
        }
    }

    public object GetSettings()
    {
        lock (_lock)
        {
            return new
            {
                codec = _codec,
                jpegQuality = _jpegQuality
            };
        }
    }

    public bool UpdateSettings(string? codec, int jpegQuality)
    {
        codec = (codec ?? "").Trim().ToLowerInvariant();
        if (codec is not ("jpeg" or "png"))
        {
            return false;
        }

        if (jpegQuality < 20 || jpegQuality > 95)
        {
            return false;
        }

        lock (_lock)
        {
            _codec = codec;
            _jpegQuality = jpegQuality;
        }

        return true;
    }

    private async Task Loop(CancellationToken ct)
    {
        var interval = TimeSpan.FromMilliseconds(1000.0 / _fps);

        while (!ct.IsCancellationRequested)
        {
            var started = DateTime.UtcNow;
            try
            {
                CaptureAndEncode();
            }
            catch
            {
            }

            var elapsed = DateTime.UtcNow - started;
            var delay = interval - elapsed;
            if (delay > TimeSpan.Zero)
            {
                await Task.Delay(delay, ct);
            }
        }
    }

    private void CaptureAndEncode()
    {
        var screens = Screen.AllScreens;
        if (screens.Length == 0) return;

        var index = _displayIndex;
        if (index < 0 || index >= screens.Length) index = 0;

        var bounds = screens[index].Bounds;

        using var bitmap = new Bitmap(bounds.Width, bounds.Height, PixelFormat.Format24bppRgb);
        using (var graphics = Graphics.FromImage(bitmap))
        {
            graphics.CopyFromScreen(bounds.Left, bounds.Top, 0, 0, bounds.Size, CopyPixelOperation.SourceCopy);
        }

        string codec;
        int quality;
        lock (_lock)
        {
            codec = _codec;
            quality = _jpegQuality;
        }

        using var ms = new MemoryStream();
        string mimeType;

        if (codec == "png")
        {
            bitmap.Save(ms, ImageFormat.Png);
            mimeType = "image/png";
        }
        else
        {
            var jpgEncoder = ImageCodecInfo.GetImageEncoders().First(x => x.MimeType == "image/jpeg");
            using var encoderParams = new EncoderParameters(1);
            encoderParams.Param[0] = new EncoderParameter(System.Drawing.Imaging.Encoder.Quality, quality);
            bitmap.Save(ms, jpgEncoder, encoderParams);
            mimeType = "image/jpeg";
        }

        var bytes = ms.ToArray();
        lock (_lock)
        {
            _latestFrame = new EncodedFrame(bytes, mimeType);
        }
    }
}

sealed record EncodedFrame(byte[] Bytes, string MimeType);

sealed class UpdateSettingsRequest
{
    public string Codec { get; set; } = "jpeg";
    public int JpegQuality { get; set; } = 75;
}
