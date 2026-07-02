using Windows.Devices.Enumeration;
using Windows.Foundation;
using Windows.Media.Audio;

namespace EchoBridge;

internal sealed class A2dpConnectionService : IDisposable
{
    private readonly SynchronizationContext _uiContext;
    private readonly Dictionary<string, AudioDeviceItem> _devices = [];
    private readonly Dictionary<string, AudioPlaybackConnection> _connections = [];
    private readonly Dictionary<string, TypedEventHandler<AudioPlaybackConnection, object>> _eventHandlers = [];
    private DeviceWatcher? _watcher;

    public A2dpConnectionService(SynchronizationContext uiContext)
    {
        _uiContext = uiContext;
    }

    public event EventHandler? DevicesChanged;
    public event EventHandler? StateChanged;

    public ConnectionState State { get; private set; } = ConnectionState.Disconnected;
    public string StatusText { get; private set; } = "未连接";
    public IReadOnlyCollection<AudioDeviceItem> Devices => _devices.Values.OrderBy(device => device.Name).ToArray();
    public IReadOnlyCollection<string> ConnectedDeviceIds => _connections.Keys.ToArray();

    public void StartScanning()
    {
        if (_watcher is not null)
        {
            return;
        }

        try
        {
            SetState(ConnectionState.Scanning, "正在扫描设备");
            var selector = AudioPlaybackConnection.GetDeviceSelector();
            _watcher = DeviceInformation.CreateWatcher(selector);
            _watcher.Added += (_, device) => Post(() => AddOrUpdateDevice(device));
            _watcher.Updated += (_, update) => Post(() => UpdateDevice(update));
            _watcher.Removed += (_, update) => Post(() => RemoveDevice(update.Id));
            _watcher.EnumerationCompleted += (_, _) => Post(() => SetState(_connections.Count == 0 ? ConnectionState.Disconnected : ConnectionState.Connected, _connections.Count == 0 ? "未连接" : "已连接"));
            _watcher.Stopped += (_, _) => Post(() => AppLog.Write("设备扫描已停止"));
            _watcher.Start();
            AppLog.Write("已启动 A2DP 设备扫描");
        }
        catch (Exception ex)
        {
            SetState(ConnectionState.Failed, "扫描设备失败");
            AppLog.Write("启动 A2DP 设备扫描失败", ex);
        }
    }

    public async Task ConnectAsync(AudioDeviceItem device)
    {
        if (_connections.ContainsKey(device.Id))
        {
            return;
        }

        SetState(ConnectionState.Connecting, $"正在连接 {device.Name}");
        AppLog.Write($"开始连接设备: {device.Name} [{device.Id}]");

        try
        {
            var connection = AudioPlaybackConnection.TryCreateFromId(device.Id);
            if (connection is null)
            {
                SetState(ConnectionState.Failed, "系统无法创建音频连接");
                AppLog.Write($"TryCreateFromId 返回 null: {device.Id}");
                return;
            }

            _connections[device.Id] = connection;
            
            var currentDeviceId = device.Id;
            TypedEventHandler<AudioPlaybackConnection, object> handler = (sender, args) => Connection_StateChanged(currentDeviceId, sender, args);
            _eventHandlers[currentDeviceId] = handler;
            connection.StateChanged += handler;

            await connection.StartAsync();
            var result = await connection.OpenAsync();

            if (result.Status == AudioPlaybackConnectionOpenResultStatus.Success)
            {
                SetState(ConnectionState.Connected, $"已连接 {device.Name}");
                AppLog.Write($"设备连接成功: {device.Name}");
                return;
            }

            var message = DescribeOpenFailure(result);
            Disconnect(device.Id);
            SetState(ConnectionState.Failed, message);
            AppLog.Write($"设备连接失败: {device.Name}, {message}");
        }
        catch (Exception ex)
        {
            Disconnect(device.Id);
            SetState(ConnectionState.Failed, "连接失败，详见日志");
            AppLog.Write($"连接设备异常: {device.Name}", ex);
        }
    }

    public void Disconnect(string deviceId)
    {
        if (!_connections.Remove(deviceId, out var connection))
        {
            return;
        }

        if (_eventHandlers.Remove(deviceId, out var handler))
        {
            try
            {
                connection.StateChanged -= handler;
            }
            catch (Exception ex)
            {
                AppLog.Write($"退订设备事件异常: {deviceId}", ex);
            }
        }

        try
        {
            connection.Dispose();
            AppLog.Write($"已断开设备: {deviceId}");
        }
        catch (Exception ex)
        {
            AppLog.Write($"断开设备异常: {deviceId}", ex);
        }

        SetState(_connections.Count == 0 ? ConnectionState.Disconnected : ConnectionState.Connected, _connections.Count == 0 ? "未连接" : "已连接");
    }

    public void DisconnectAll()
    {
        foreach (var id in _connections.Keys.ToArray())
        {
            Disconnect(id);
        }
    }



    public async Task ReconnectAsync(string deviceId)
    {
        Disconnect(deviceId);

        if (!_devices.TryGetValue(deviceId, out var device))
        {
            try
            {
                var info = await DeviceInformation.CreateFromIdAsync(deviceId);
                device = new AudioDeviceItem(info.Id, info.Name);
                _devices[device.Id] = device;
                RaiseDevicesChanged();
            }
            catch (Exception ex)
            {
                SetState(ConnectionState.Failed, "找不到上次连接的设备");
                AppLog.Write($"按 ID 恢复设备失败: {deviceId}", ex);
                return;
            }
        }

        await ConnectAsync(device);
    }

    public void ForceRefresh()
    {
        try
        {
            AppLog.Write("正在执行 A2DP 设备强制物理重新扫描...");
            if (_watcher is not null)
            {
                try
                {
                    if (_watcher.Status is DeviceWatcherStatus.Started or DeviceWatcherStatus.EnumerationCompleted)
                    {
                        _watcher.Stop();
                    }
                }
                catch { }
                _watcher = null;
            }
            _devices.Clear();
            RaiseDevicesChanged();
            StartScanning();
        }
        catch (Exception ex)
        {
            AppLog.Write("强制物理刷新 A2DP 设备失败", ex);
        }
    }

    public void Dispose()
    {
        DisconnectAll();
        if (_watcher is not null)
        {
            try
            {
                if (_watcher.Status is DeviceWatcherStatus.Started or DeviceWatcherStatus.EnumerationCompleted)
                {
                    _watcher.Stop();
                }
            }
            catch (Exception ex)
            {
                AppLog.Write("停止设备扫描失败", ex);
            }
        }
    }

    private static string DescribeOpenFailure(AudioPlaybackConnectionOpenResult result)
    {
        return result.Status switch
        {
            AudioPlaybackConnectionOpenResultStatus.RequestTimedOut => "连接请求超时",
            AudioPlaybackConnectionOpenResultStatus.DeniedBySystem => "系统拒绝连接",
            AudioPlaybackConnectionOpenResultStatus.UnknownFailure => $"未知连接失败: 0x{result.ExtendedError.HResult:X8}",
            _ => $"连接失败: {result.Status}"
        };
    }

    private void AddOrUpdateDevice(DeviceInformation device)
    {
        _devices[device.Id] = new AudioDeviceItem(device.Id, device.Name);
        RaiseDevicesChanged();
    }

    private void UpdateDevice(DeviceInformationUpdate update)
    {
        if (_devices.TryGetValue(update.Id, out var existing))
        {
            _devices[update.Id] = existing;
            RaiseDevicesChanged();
        }
    }

    private void RemoveDevice(string deviceId)
    {
        _devices.Remove(deviceId);
        RaiseDevicesChanged();
    }

    private void Connection_StateChanged(string deviceId, AudioPlaybackConnection sender, object args)
    {
        AudioPlaybackConnectionState? state = null;

        try
        {
            state = sender.State;
        }
        catch (Exception ex)
        {
            AppLog.Write($"读取连接状态失败: {deviceId}", ex);
        }

        Post(() =>
        {
            AppLog.Write($"连接状态变化: {deviceId}, {state?.ToString() ?? "未知状态"}");

            // 忽略系统由于抢占通道等触发的 Closed 信号，以避免在通道状态发生短暂抖动时自动去勾/断开设备。
            // 仅当状态为 Opened 时确立连接状态，由用户在托盘菜单主动控制断开。
            if (state == AudioPlaybackConnectionState.Opened)
            {
                SetState(ConnectionState.Connected, "已连接");
            }
        });
    }

    private void SetState(ConnectionState state, string text)
    {
        State = state;
        StatusText = text;
        StateChanged?.Invoke(this, EventArgs.Empty);
    }

    private void RaiseDevicesChanged() => DevicesChanged?.Invoke(this, EventArgs.Empty);

    private void Post(Action action)
    {
        _uiContext.Post(_ =>
        {
            try
            {
                action();
            }
            catch (Exception ex)
            {
                AppLog.Write("UI 回调执行失败", ex);
            }
        }, null);
    }
}
