using AsmResolver.PE;
using AsmResolver.PE.Builder;
using AsmResolver.PE.File;
using AsmResolver.PE.Imports;
using System;
using System.IO;
using System.Diagnostics;
using System.Threading;

namespace Droute.Core
{
    internal static class PatchFiles
    {
        public static void CopySystemProxy(string destPath)
        {
            if (!Environment.Is64BitOperatingSystem)
                throw new PlatformNotSupportedException("Droute requires 64-bit Windows.");

            string sourceFolder = Environment.Is64BitProcess
                ? Environment.GetFolderPath(Environment.SpecialFolder.System)
                : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "Sysnative");

            File.Copy(Path.Combine(sourceFolder, PatchInstaller.ProxyFileName), destPath, true);
        }

        public static void AddLibraryImport(string filePath)
        {
            var peFile = PEFile.FromFile(filePath);
            var peImage = PEImage.FromFile(peFile);

            var nativeLibrary = new ImportedModule(PatchInstaller.NativeLibraryFileName);
            nativeLibrary.Symbols.Add(new ImportedSymbol(0, "DllMain"));
            peImage.Imports.Add(nativeLibrary);

            var builder = new TemplatedPEFileBuilder()
            {
                TrampolineImports = true
            };

            peFile = builder.CreateFile(peImage);
            peFile.Write(filePath);
        }

        public static void PublishStagedFile(string stagedPath, string destinationPath)
        {
            if (string.IsNullOrEmpty(stagedPath))
                throw new ArgumentException("Staged file path is required.", nameof(stagedPath));

            if (string.IsNullOrEmpty(destinationPath))
                throw new ArgumentException("Destination file path is required.", nameof(destinationPath));

            if (!File.Exists(stagedPath))
                throw new FileNotFoundException("Staged file was not created.", stagedPath);

            var clock = Stopwatch.StartNew();
            for (; ; )
            {
                try
                {
                    if (File.Exists(destinationPath))
                        File.Replace(stagedPath, destinationPath, null, true);
                    else
                        File.Move(stagedPath, destinationPath);

                    return;
                }
                catch (IOException ex) when ((ex.HResult & 0xffff) == 32 || (ex.HResult & 0xffff) == 33)
                {
                    if (clock.ElapsedMilliseconds >= 5000)
                        throw;

                    Thread.Sleep(100);
                }
            }
        }
    }
}
