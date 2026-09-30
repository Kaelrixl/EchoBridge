namespace EchoBridge;

internal static class AppLog
{
    private static readonly object SyncRoot = new();

    public static string LogPath => Path.Combine(AppContext.BaseDirectory, "echobridge.log");

    public static void Write(string message, Exception? exception = null)
    {
        // 停止向物理磁盘写日志，以实现无日志文件运行
    }
}
