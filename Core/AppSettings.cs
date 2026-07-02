using System.Text.Json;

namespace EchoBridge;

internal sealed class AppSettings
{
    public bool ReconnectOnStart { get; set; }
    public List<string> LastDeviceIds { get; set; } = [];
    public bool StartWithWindows { get; set; }

    public static string SettingsPath => Path.Combine(AppContext.BaseDirectory, "EchoBridge.json");

    public static AppSettings Load()
    {
        try
        {
            if (!File.Exists(SettingsPath))
            {
                return new AppSettings();
            }

            var json = File.ReadAllText(SettingsPath);
            return JsonSerializer.Deserialize<AppSettings>(json) ?? new AppSettings();
        }
        catch (Exception ex)
        {
            AppLog.Write("读取配置失败", ex);
            return new AppSettings();
        }
    }

    public void Save()
    {
        try
        {
            var json = JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true });
            File.WriteAllText(SettingsPath, json);
        }
        catch (Exception ex)
        {
            AppLog.Write("保存配置失败", ex);
        }
    }
}
