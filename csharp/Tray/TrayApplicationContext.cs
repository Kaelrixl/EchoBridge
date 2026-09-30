using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Runtime.InteropServices;

namespace EchoBridge;

internal sealed class TrayApplicationContext : ApplicationContext
{
    private readonly NotifyIcon _notifyIcon;
    private readonly ContextMenuStrip _menu = new();
    private readonly AppSettings _settings;
    private readonly A2dpConnectionService _connectionService;
    private readonly ToolStripMenuItem _devicesRoot = new("设备");
    private readonly ToolStripMenuItem _reconnectOnStartItem = new("启动自动重连") { CheckOnClick = true };
    private readonly ToolStripMenuItem _startWithWindowsItem = new("开机自启动") { CheckOnClick = true };
    private readonly ToolStripMenuItem _statusItem = new("") { Name = "StatusIndicatorItem", Enabled = false };
    public static readonly List<int> DeviceStatuses = [0, 0, 0, 0];

    public TrayApplicationContext()
    {
        _settings = AppSettings.Load();
        _reconnectOnStartItem.Checked = _settings.ReconnectOnStart;
        
        // 读取并同步自启动的实际注册表状态
        _startWithWindowsItem.Checked = IsStartupEnabled();
        if (_settings.StartWithWindows && !_startWithWindowsItem.Checked)
        {
            SetStartup(true);
            _startWithWindowsItem.Checked = IsStartupEnabled();
        }

        // 设置菜单样式：微软雅黑字体、适当的字号与渲染器
        _menu.Font = new Font("Microsoft YaHei UI", 9F, FontStyle.Regular, GraphicsUnit.Point);
        _menu.Renderer = new ModernToolStripRenderer();
        _menu.ShowImageMargin = false;
        _menu.ShowCheckMargin = true;
        _menu.Opening += DropDown_Opening;
        _menu.Padding = new Padding(0); // 移除主菜单容器本身的多余空白白边

        var uiContext = SynchronizationContext.Current ?? new WindowsFormsSynchronizationContext();
        _connectionService = new A2dpConnectionService(uiContext);
        _connectionService.DevicesChanged += (_, _) => RebuildDeviceMenu();
        _connectionService.StateChanged += (_, _) => UpdateStatus();

        _notifyIcon = new NotifyIcon
        {
            Icon = CreateAppIcon(),
            Text = "EchoBridge",
            Visible = true,
            ContextMenuStrip = _menu
        };

        BuildMenu();
        _connectionService.StartScanning();
        _ = RestoreConnectionsAsync();
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _connectionService.Dispose();
            _notifyIcon.Visible = false;
            _notifyIcon.Dispose();
            _menu.Dispose();
        }

        base.Dispose(disposing);
    }

    private void BuildMenu()
    {
        _menu.Items.Clear();
        _menu.Items.Add(_statusItem);
        _statusItem.Padding = new Padding(0, 6, 0, 6);
        _menu.Items.Add(new ToolStripSeparator());
        _menu.Items.Add(_devicesRoot);
        _menu.Items.Add(new ToolStripMenuItem("刷新设备", null, (_, _) => _connectionService.ForceRefresh()));
        _menu.Items.Add(new ToolStripMenuItem("断开全部", null, (_, _) => _connectionService.DisconnectAll()));
        _menu.Items.Add(new ToolStripSeparator());
        _menu.Items.Add(_reconnectOnStartItem);
        _menu.Items.Add(_startWithWindowsItem);
        _menu.Items.Add(new ToolStripMenuItem("蓝牙设置", null, (_, _) => OpenUri("ms-settings:bluetooth")));
        _menu.Items.Add(new ToolStripSeparator());
        _menu.Items.Add(new ToolStripMenuItem("退出", null, (_, _) => ExitApplication()));

        _reconnectOnStartItem.CheckedChanged += (_, _) =>
        {
            _settings.ReconnectOnStart = _reconnectOnStartItem.Checked;
            SaveSettings();
        };

        _startWithWindowsItem.CheckedChanged += (_, _) =>
        {
            _settings.StartWithWindows = _startWithWindowsItem.Checked;
            SetStartup(_settings.StartWithWindows);
            SaveSettings();
        };

        ApplyModernPadding(_menu.Items);
        RebuildDeviceMenu();
    }

    private void RebuildDeviceMenu()
    {
        _devicesRoot.DropDownItems.Clear();

        var devices = _connectionService.Devices.ToArray();
        // 指示灯优先展示已连接设备，剩余位置展示其他已发现设备。
        var indicatorDevices = devices.OrderByDescending(device =>
            _connectionService.ConnectedDeviceIds.Contains(device.Id)).Take(A2dpConnectionService.MaxConnections).ToArray();

        // 更新设备状态指示列表
        for (int i = 0; i < 4; i++)
        {
            if (i < indicatorDevices.Length)
            {
                var device = indicatorDevices[i];
                var connected = _connectionService.ConnectedDeviceIds.Contains(device.Id);
                DeviceStatuses[i] = connected ? 2 : 1; // 2 = 已连接（绿），1 = 已配对但未连接（蓝）
            }
            else
            {
                DeviceStatuses[i] = 0; // 0 = 空置槽（灰）
            }
        }

        if (devices.Length == 0)
        {
            _devicesRoot.DropDownItems.Add(new ToolStripMenuItem("未发现 A2DP 设备") { Enabled = false });
            return;
        }

        foreach (var device in devices)
        {
            var connected = _connectionService.ConnectedDeviceIds.Contains(device.Id);
            var item = new ToolStripMenuItem(device.ToString())
            {
                Checked = connected,
                Enabled = connected || _connectionService.CanConnect
            };

            item.Click += async (_, _) =>
            {
                if (connected)
                {
                    _connectionService.Disconnect(device.Id);
                }
                else
                {
                    await _connectionService.ConnectAsync(device);
                }

                SaveSettings();
                RebuildDeviceMenu();
            };

            _devicesRoot.DropDownItems.Add(item);
        }

        _devicesRoot.DropDown.Font = _menu.Font;
        _devicesRoot.DropDown.Renderer = _menu.Renderer;
        if (_devicesRoot.DropDown is ToolStripDropDownMenu deviceMenu)
        {
            deviceMenu.ShowImageMargin = false;
            deviceMenu.ShowCheckMargin = true;
        }
        _devicesRoot.DropDown.Opening -= DropDown_Opening; // 先解绑旧句柄，防止每次 RebuildDeviceMenu 都累积重复订阅
        _devicesRoot.DropDown.Opening += DropDown_Opening;
        _devicesRoot.DropDown.Padding = new Padding(0); // 移除二级菜单容器本身的多余空白白边
        ApplyModernPadding(_devicesRoot.DropDownItems);
    }

    private void UpdateStatus()
    {
        // 托盘悬停依旧显示详细连接文本方便查阅
        _notifyIcon.Text = _connectionService.StatusText.Length > 63 ? _connectionService.StatusText[..63] : _connectionService.StatusText;
        RebuildDeviceMenu();
        _menu.Invalidate(); // 使主菜单重绘以即时重绘 4 个圆形指示灯
    }

    private async Task RestoreConnectionsAsync()
    {
        if (!_settings.ReconnectOnStart || _settings.LastDeviceIds.Count == 0)
        {
            return;
        }

        var deviceIds = _settings.LastDeviceIds.ToArray();
        for (int i = 0; i < deviceIds.Length; i++)
        {
            await _connectionService.ReconnectAsync(deviceIds[i]);
            if (i < deviceIds.Length - 1)
            {
                // 延迟 1500 毫秒（1.5秒）。给前一个设备的 Windows A2DP 音频路由切换留出充足缓冲，解决双设备并发抢占死锁无声音 Bug
                await Task.Delay(1500);
            }
        }
    }

    private static void OpenUri(string uri)
    {
        try
        {
            Process.Start(new ProcessStartInfo(uri) { UseShellExecute = true });
        }
        catch (Exception ex)
        {
            AppLog.Write($"打开 URI 失败: {uri}", ex);
        }
    }

    private void ExitApplication()
    {
        SaveSettings();
        ExitThread();
    }

    private void SaveSettings()
    {
        _settings.LastDeviceIds = _connectionService.ConnectedDeviceIds.ToList();
        _settings.Save();
    }

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int attrValue, int attrSize);

    private const int DWMWA_WINDOW_CORNER_PREFERENCE = 33;
    private const int DWMWCP_ROUNDSMALL = 3; // 精致小圆角，适合菜单

    [DllImport("gdi32.dll")]
    private static extern IntPtr CreateRoundRectRgn(int nLeftRect, int nTopRect, int nRightRect, int nBottomRect, int nWidthEllipse, int nHeightEllipse);

    private void DropDown_Opening(object? sender, System.ComponentModel.CancelEventArgs e)
    {
        if (sender is ToolStripDropDown dropdown)
        {
            // 1. 强制锁死图像列和勾选列的边距设定，防止被 Windows 底层在动态加载设备时重置回大空白
            if (dropdown is ToolStripDropDownMenu dropdownMenu)
            {
                dropdownMenu.ShowImageMargin = false;
                dropdownMenu.ShowCheckMargin = true;
            }

            // 2. 启用系统级 DWM 原生小圆角
            //    在 Opening（菜单首帧渲染前）而非 Opened（已出现后）调用，
            //    确保圆角属性在菜单第一帧可见前已经生效，彻底消除因"先方角出现再变圆角"导致的重影闪烁
            var hwnd = dropdown.Handle;
            int cornerPreference = DWMWCP_ROUNDSMALL;
            DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, ref cornerPreference, sizeof(int));

            // 3. 如果是二级子菜单，往左移 6 像素，消除与主菜单之间的物理黑线缝隙，使其完全无缝贴靠
            //    同样必须在 Opening 而非 Opened 中调整，否则菜单会先出现在旧位置再跳动，产生重影
            if (dropdown.OwnerItem is ToolStripMenuItem)
            {
                dropdown.Left -= 6;
            }
        }
    }

    private static void ApplyModernPadding(ToolStripItemCollection items)
    {
        foreach (ToolStripItem item in items)
        {
            if (item is ToolStripMenuItem menuItem)
            {
                // 无子菜单项右 Padding 改为负 26 像素，硬性缩窄自适应宽度，彻底砍掉右侧大片空白
                if (menuItem.DropDownItems.Count == 0)
                {
                    menuItem.Padding = new Padding(1, 1, -26, 1);
                }
                else
                {
                    menuItem.Padding = new Padding(1, 1, 1, 1);
                }
            }
        }
    }

    internal static Icon CreateAppIcon()
    {
        try
        {
            var assembly = System.Reflection.Assembly.GetExecutingAssembly();
            using (var stream = assembly.GetManifestResourceStream("EchoBridge.Assets.EchoBridge.ico"))
            {
                if (stream != null)
                {
                    return new Icon(stream, 32, 32);
                }
            }
        }
        catch (Exception ex)
        {
            AppLog.Write("载入嵌入的 app.ico 失败，采用手绘备用", ex);
        }

        using var bitmap = new Bitmap(32, 32);
        using (var g = Graphics.FromImage(bitmap))
        {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.Clear(Color.Transparent);

            // 1. 绘制弯弯跨越的优雅抛物线桥梁
            using (var pen = new Pen(Color.FromArgb(160, 160, 160), 2F)) // 优雅中灰色桥身
            {
                g.DrawBezier(pen, 6, 22, 12, 10, 20, 10, 26, 22);
            }

            // 2. 绘制左下角蓝色发送端点
            using (var blueBrush = new SolidBrush(Color.FromArgb(33, 150, 243))) // 就绪亮蓝
            {
                g.FillEllipse(blueBrush, 4, 19, 5, 5);
            }

            // 3. 绘制右下角绿色接收端点
            using (var greenBrush = new SolidBrush(Color.FromArgb(76, 175, 80))) // 经典绿色
            {
                g.FillEllipse(greenBrush, 23, 19, 5, 5);
            }

            // 4. 在上方绘制两道对称精致的声波同心弧线 (表示 Echo 回声)
            using (var wavePen = new Pen(Color.FromArgb(120, 120, 120), 1.5F))
            {
                // 绘制内层声波弧
                g.DrawArc(wavePen, 10, 6, 12, 12, 210, 120);
                // 绘制外层声波弧
                g.DrawArc(wavePen, 7, 2, 18, 18, 210, 120);
            }
        }

        IntPtr hIcon = bitmap.GetHicon();
        return Icon.FromHandle(hIcon);
    }

    private const string RunRegistryKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string AppRegistryValueName = "EchoBridge";

    private static void SetStartup(bool enable)
    {
        try
        {
            using var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(RunRegistryKey, true);
            if (key != null)
            {
                if (enable)
                {
                    key.SetValue(AppRegistryValueName, $"\"{Application.ExecutablePath}\"");
                }
                else
                {
                    key.DeleteValue(AppRegistryValueName, false);
                }
            }
        }
        catch (Exception ex)
        {
            AppLog.Write("设置开机自启动失败", ex);
        }
    }

    private static bool IsStartupEnabled()
    {
        try
        {
            using var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(RunRegistryKey, false);
            if (key != null)
            {
                var value = key.GetValue(AppRegistryValueName) as string;
                return !string.IsNullOrEmpty(value) && value.Contains(Application.ExecutablePath);
            }
        }
        catch (Exception ex)
        {
            AppLog.Write("检查开机自启动状态失败", ex);
        }
        return false;
    }
}

internal sealed class ModernToolStripRenderer : ToolStripProfessionalRenderer
{
    public ModernToolStripRenderer() : base(new ModernColorTable())
    {
    }

    protected override void OnRenderMenuItemBackground(ToolStripItemRenderEventArgs e)
    {
        if (e.Item.Name == "StatusIndicatorItem")
        {
            // 1. 填充第一行项的纯白背景（与菜单融为一体）
            var rect = new Rectangle(Point.Empty, e.Item.Size);
            using (var bgBrush = new SolidBrush(Color.FromArgb(250, 250, 250)))
            {
                e.Graphics.FillRectangle(bgBrush, rect);
            }

            e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;

            // 2. 依次手绘 4 个状态圆指示灯
            float dotDiameter = 10f; // 指示灯圆直径（10 像素大小非常精致高档）
            float dotSpacing = 16f;  // 两个圆之间的间距
            float totalWidth = dotDiameter * 4 + dotSpacing * 3; // 40 + 48 = 88 像素
            float startX = (rect.Width - totalWidth) / 2f;
            float centerY = (rect.Height - dotDiameter) / 2f;

            for (int i = 0; i < 4; i++)
            {
                float x = startX + i * (dotDiameter + dotSpacing);
                int status = i < TrayApplicationContext.DeviceStatuses.Count ? TrayApplicationContext.DeviceStatuses[i] : 0;

                Color dotColor;
                if (status == 2)
                {
                    dotColor = Color.FromArgb(76, 175, 80); // 2 = 已连接（经典绿色）
                }
                else if (status == 1)
                {
                    dotColor = Color.FromArgb(33, 150, 243); // 1 = 已配对但未连接（亮丽蓝色）
                }
                else
                {
                    dotColor = Color.FromArgb(228, 228, 228); // 0 = 空置位（柔和淡灰色圈）
                }

                // 绘制指示圆
                using var brush = new SolidBrush(dotColor);
                e.Graphics.FillEllipse(brush, x, centerY, dotDiameter, dotDiameter);
            }
            return;
        }

        if (e.Item.Selected && e.Item.Enabled)
        {
            var rect = new Rectangle(Point.Empty, e.Item.Size);
            using var brush = new SolidBrush(Color.FromArgb(235, 240, 245)); // 悬停淡雅浅蓝
            e.Graphics.FillRectangle(brush, rect);
        }

        // 绘制未选中（Unchecked）可勾选项的虚空框描边，方便用户识别哪些项是支持复选的
        if (e.Item is ToolStripMenuItem menuItem && menuItem.Enabled)
        {
            bool isCheckable = menuItem.CheckOnClick || 
                              (menuItem.Owner is ToolStripDropDown drop && drop.OwnerItem?.Text == "设备");
            
            if (isCheckable && !menuItem.Checked)
            {
                e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
                float boxSize = 12f;
                float boxX = 10f; // 与 OnRenderItemCheck 的 X 轴位置完美重合
                float boxY = (menuItem.Height - boxSize) / 2f - 2f; // 与 OnRenderItemCheck 的 Y 轴位置完美重合

                // 采用淡雅浅灰色圆角描边，凸显精致感
                using (var pen = new Pen(Color.FromArgb(200, 200, 200), 1F))
                using (var path = GetRoundRectPath(boxX, boxY, boxSize, boxSize, 2))
                {
                    e.Graphics.DrawPath(pen, path);
                }
            }
        }
    }

    protected override void OnRenderSeparator(ToolStripSeparatorRenderEventArgs e)
    {
        var rect = new Rectangle(Point.Empty, e.Item.Size);
        using var brush = new SolidBrush(Color.FromArgb(250, 250, 250)); // 一体化背景
        e.Graphics.FillRectangle(brush, rect);

        using var pen = new Pen(Color.FromArgb(228, 228, 228)); // 极淡灰色分割线
        var y = rect.Height / 2;
        e.Graphics.DrawLine(pen, 10, y, rect.Width - 10, y);
    }

    protected override void OnRenderToolStripBorder(ToolStripRenderEventArgs e)
    {
        var rect = new Rectangle(Point.Empty, e.ToolStrip.Size);
        e.Graphics.SmoothingMode = SmoothingMode.AntiAlias; // 开启高精度抗锯齿

        using var pen = new Pen(Color.FromArgb(218, 218, 218)); // 极细灰色圆角描边线 (圆角半径设为 5，与剪裁 Region 像素微米级契合对齐以完全消灭毛边)
        using var path = GetRoundRectPath(0, 0, rect.Width - 1, rect.Height - 1, 5);
        e.Graphics.DrawPath(pen, path);
    }

    protected override void OnRenderItemCheck(ToolStripItemImageRenderEventArgs e)
    {
        // 渲染高拟真、极其清晰光滑的 `✅` 绿底白勾精致小图标
        var rect = e.ImageRectangle;
        e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;

        // 1. 计算绿色底色块区域，设计为 12x12 尺寸。我们进行 X 轴右平移 4px 靠近文字，并将 Y 轴向上偏移 2px 对齐文字水平居中线！
        float boxSize = 12f;
        float boxX = rect.Left + (rect.Width - boxSize) / 2f + 4f;
        float boxY = rect.Top + (rect.Height - boxSize) / 2f - 2f;

        // 2. 使用圆角半径为 2 绘制优雅的圆角绿色方块背景
        using var brush = new SolidBrush(Color.FromArgb(76, 175, 80)); // 经典 ✅ 绿色
        using var path = GetRoundRectPath(boxX, boxY, boxSize, boxSize, 2);
        e.Graphics.FillPath(brush, path);

        // 3. 在绿色底块内手绘极细的白色对勾 (1.5px 粗细)
        using var pen = new Pen(Color.White, 1.5F);
        
        float startX = boxX + 2.2f;
        float startY = boxY + 3.0f;

        e.Graphics.DrawLine(pen, startX + 1.0f, startY + 3.0f, startX + 3.0f, startY + 5.0f);
        e.Graphics.DrawLine(pen, startX + 3.0f, startY + 5.0f, startX + 7.5f, startY + 0.5f);
    }

    private static GraphicsPath GetRoundRectPath(float x, float y, float width, float height, float radius)
    {
        var path = new GraphicsPath();
        var diameter = radius * 2;
        path.AddArc(x, y, diameter, diameter, 180, 90);
        path.AddArc(x + width - diameter, y, diameter, diameter, 270, 90);
        path.AddArc(x + width - diameter, y + height - diameter, diameter, diameter, 0, 90);
        path.AddArc(x, y + height - diameter, diameter, diameter, 90, 90);
        path.CloseAllFigures();
        return path;
    }
}

internal sealed class ModernColorTable : ProfessionalColorTable
{
    public override Color ToolStripDropDownBackground => Color.FromArgb(250, 250, 250); // 主体纯净背景
    public override Color ImageMarginGradientBegin => Color.FromArgb(250, 250, 250);
    public override Color ImageMarginGradientMiddle => Color.FromArgb(250, 250, 250);
    public override Color ImageMarginGradientEnd => Color.FromArgb(250, 250, 250);
    public override Color MenuBorder => Color.FromArgb(218, 218, 218);
    public override Color MenuItemSelected => Color.FromArgb(235, 240, 245);
    public override Color MenuItemSelectedGradientBegin => Color.FromArgb(235, 240, 245);
    public override Color MenuItemSelectedGradientEnd => Color.FromArgb(235, 240, 245);

    // 重新配置复选框背景，使其在浅白色背景下浑然一体
    public override Color CheckBackground => Color.FromArgb(250, 250, 250);
    public override Color CheckSelectedBackground => Color.FromArgb(235, 240, 245);
    public override Color CheckPressedBackground => Color.FromArgb(235, 240, 245);
}
