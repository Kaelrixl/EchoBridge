namespace EchoBridge;

internal static class Program
{
    [STAThread]
    private static void Main()
    {
        // 自动检查生成 app.ico 以便 MSBuild 编译器嵌入到 EXE 文件物理属性中
        if (System.IO.File.Exists("Assets/EchoBridge.png"))
        {
            try
            {
                byte[] originalPngBytes = System.IO.File.ReadAllBytes("Assets/EchoBridge.png");
                System.Collections.Generic.List<byte[]> pngDataList = new System.Collections.Generic.List<byte[]>();

                int[] sizes = new int[] { 16, 32, 48, 128 };
                using (var originalBmp = new System.Drawing.Bitmap("Assets/EchoBridge.png"))
                {
                    using (var ia = new System.Drawing.Imaging.ImageAttributes())
                    {
                        ia.SetWrapMode(System.Drawing.Drawing2D.WrapMode.TileFlipXY);
                        foreach (int sz in sizes)
                        {
                            System.Drawing.Bitmap currentBmp = originalBmp;
                            int currentSize = originalBmp.Width;

                            while (currentSize > sz * 2)
                            {
                                int nextSize = currentSize / 2;
                                var tempBmp = new System.Drawing.Bitmap(nextSize, nextSize, System.Drawing.Imaging.PixelFormat.Format32bppArgb);
                                using (var gTemp = System.Drawing.Graphics.FromImage(tempBmp))
                                {
                                    gTemp.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
                                    gTemp.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.HighQuality;
                                    gTemp.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.HighQuality;
                                    gTemp.CompositingQuality = System.Drawing.Drawing2D.CompositingQuality.HighQuality;
                                    gTemp.Clear(System.Drawing.Color.Transparent);
                                    gTemp.DrawImage(currentBmp, new System.Drawing.Rectangle(0, 0, nextSize, nextSize), 0, 0, currentSize, currentSize, System.Drawing.GraphicsUnit.Pixel, ia);
                                }

                                if (currentBmp != originalBmp)
                                {
                                    currentBmp.Dispose();
                                }
                                currentBmp = tempBmp;
                                currentSize = nextSize;
                            }

                            // 强制 32bppArgb 格式保留 Alpha 透明度并做最后一步缩放
                            using (var scaledBmp = new System.Drawing.Bitmap(sz, sz, System.Drawing.Imaging.PixelFormat.Format32bppArgb))
                            {
                                using (var g = System.Drawing.Graphics.FromImage(scaledBmp))
                                {
                                    g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
                                    g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.HighQuality;
                                    g.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.HighQuality;
                                    g.CompositingQuality = System.Drawing.Drawing2D.CompositingQuality.HighQuality;
                                    g.Clear(System.Drawing.Color.Transparent);

                                    g.DrawImage(currentBmp, new System.Drawing.Rectangle(0, 0, sz, sz), 0, 0, currentSize, currentSize, System.Drawing.GraphicsUnit.Pixel, ia);
                                }

                                if (currentBmp != originalBmp)
                                {
                                    currentBmp.Dispose();
                                }

                                using (var ms = new System.IO.MemoryStream())
                                {
                                    scaledBmp.Save(ms, System.Drawing.Imaging.ImageFormat.Png);
                                    pngDataList.Add(ms.ToArray());
                                }
                            }
                        }
                    }
                }

                // Append original 256x256 PNG bytes as the final high-res layer
                pngDataList.Add(originalPngBytes);

                int count = pngDataList.Count;
                System.Collections.Generic.List<byte> header = new System.Collections.Generic.List<byte>();

                // ICONHEADER (6 bytes)
                header.Add(0); header.Add(0); // Reserved
                header.Add(1); header.Add(0); // Type = 1 (Icon)
                header.Add((byte)count); header.Add(0); // Count = 4

                int offset = 6 + 16 * count;
                byte[] wVals = new byte[] { 16, 32, 48, 128, 0 };
                byte[] hVals = new byte[] { 16, 32, 48, 128, 0 };

                for (int i = 0; i < count; i++)
                {
                    byte[] bytes = pngDataList[i];
                    int len = bytes.Length;

                    // ICONDIRENTRY (16 bytes)
                    header.Add(wVals[i]);
                    header.Add(hVals[i]);
                    header.Add(0);
                    header.Add(0);
                    header.Add(1); header.Add(0);
                    header.Add(32); header.Add(0);

                    // BytesInRes (4 bytes)
                    header.Add((byte)(len & 0xFF));
                    header.Add((byte)((len >> 8) & 0xFF));
                    header.Add((byte)((len >> 16) & 0xFF));
                    header.Add((byte)((len >> 24) & 0xFF));

                    // ImageOffset (4 bytes)
                    header.Add((byte)(offset & 0xFF));
                    header.Add((byte)((offset >> 8) & 0xFF));
                    header.Add((byte)((offset >> 16) & 0xFF));
                    header.Add((byte)((offset >> 24) & 0xFF));

                    offset += len;
                }

                using (var fs = new System.IO.FileStream("Assets/EchoBridge.ico", System.IO.FileMode.Create))
                {
                    byte[] headerBytes = header.ToArray();
                    fs.Write(headerBytes, 0, headerBytes.Length);
                    foreach (var bytes in pngDataList)
                    {
                        fs.Write(bytes, 0, bytes.Length);
                    }
                }
            }
            catch (Exception ex)
            {
                AppLog.Write("自动从 EchoBridge.png 转换 EchoBridge.ico 失败", ex);
            }
        }
        // 只在开发态检测到 atom.png 时执行编译前 ico 转换

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
