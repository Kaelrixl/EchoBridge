namespace EchoBridge;

internal static class Program
{
    [STAThread]
    private static void Main()
    {
        Application.ThreadException += (_, args) => AppLog.Write("WinForms 线程异常", args.Exception);
        AppDomain.CurrentDomain.UnhandledException += (_, args) =>
        {
            if (args.ExceptionObject is Exception ex)
            {
                AppLog.Write("未处理异常", ex);
            }
        };

        using var mutex = new Mutex(true, "EchoBridge.SingleInstance", out var createdNew);
        if (!createdNew)
        {
            MessageBox.Show("EchoBridge 已在运行。", "EchoBridge", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }

        ApplicationConfiguration.Initialize();
        using var context = new TrayApplicationContext();
        Application.Run(context);
    }
}
