using AsmResolver.PE;
using AsmResolver.PE.Builder;
using AsmResolver.PE.File;
using AsmResolver.PE.Imports;
using System;
using System.IO;
using System.Threading;

namespace Droute.Core
{
    public static class PatchManager
    {
        public const string MAIN_PROXY_DLL = "version.dll";
        public const string MAIN_PAYLOAD_DLL = "droute.dll";

        public enum ArchitectureBitness
        {
            Auto,
            Force64,
            Force32
        }

        public static void DuplicateProxy(string destPath, ArchitectureBitness bitness = ArchitectureBitness.Force64)
        {
            string windowsPath = Environment.GetFolderPath(Environment.SpecialFolder.Windows);
            string sourceFolder;

            switch (bitness)
            {
                case ArchitectureBitness.Force64:
                    if (Environment.Is64BitOperatingSystem && !Environment.Is64BitProcess)
                        sourceFolder = Path.Combine(windowsPath, "Sysnative");
                    else
                        sourceFolder = Environment.GetFolderPath(Environment.SpecialFolder.System);
                    break;

                case ArchitectureBitness.Force32:
                    if (Environment.Is64BitOperatingSystem)
                        sourceFolder = Path.Combine(windowsPath, "SysWOW64");
                    else
                        sourceFolder = Environment.GetFolderPath(Environment.SpecialFolder.System);
                    break;

                case ArchitectureBitness.Auto:
                default:
                    sourceFolder = Environment.GetFolderPath(Environment.SpecialFolder.System);
                    break;
            }

            string source = Path.Combine(sourceFolder, MAIN_PROXY_DLL);

            if (!File.Exists(source))
                throw new FileNotFoundException($"Required system component is missing: {source}");

            File.Copy(source, destPath, true);
        }

        public static void ApplyPEPatch(string filePath)
        {
            var peFile = PEFile.FromFile(filePath);
            var peImage = PEImage.FromFile(peFile);

            var myDll = new ImportedModule(MAIN_PAYLOAD_DLL);
            myDll.Symbols.Add(new ImportedSymbol(0, "DllMain"));
            peImage.Imports.Add(myDll);

            var builder = new TemplatedPEFileBuilder()
            {
                TrampolineImports = true
            };

            peFile = builder.CreateFile(peImage);
            peFile.Write(filePath);
        }

        public static void DeleteFile(string filePath)
        {
            if (File.Exists(filePath))
                File.Delete(filePath);
        }

        public static void PublishStagedFile(string stagedPath, string destinationPath)
        {
            if (string.IsNullOrEmpty(stagedPath))
                throw new ArgumentException("Staged file path is required.", nameof(stagedPath));

            if (string.IsNullOrEmpty(destinationPath))
                throw new ArgumentException("Destination file path is required.", nameof(destinationPath));

            if (!File.Exists(stagedPath))
                throw new FileNotFoundException("Staged file was not created.", stagedPath);

            if (File.Exists(destinationPath))
                File.Replace(stagedPath, destinationPath, null, true);
            else
                File.Move(stagedPath, destinationPath);
        }

        public static void DeleteStagedFile(string path)
        {
            if (string.IsNullOrEmpty(path))
                return;

            try { if (File.Exists(path)) File.Delete(path); }
            catch { }
        }

        public static void WaitForFilesAvailable(string[] paths, int timeoutMilliseconds)
        {
            if (paths == null)
                throw new ArgumentNullException(nameof(paths));

            if (timeoutMilliseconds < 0)
                throw new ArgumentOutOfRangeException(nameof(timeoutMilliseconds));

            DateTime deadline = DateTime.UtcNow.AddMilliseconds(timeoutMilliseconds);
            Exception lastException = null;

            do
            {
                bool allAvailable = true;

                foreach (string path in paths)
                {
                    if (string.IsNullOrEmpty(path) || !File.Exists(path))
                        continue;

                    try
                    {
                        using (File.Open(path, FileMode.Open, FileAccess.ReadWrite, FileShare.None)) { }
                    }
                    catch (IOException ex)
                    {
                        allAvailable = false;
                        lastException = ex;
                        break;
                    }
                    catch (UnauthorizedAccessException ex)
                    {
                        allAvailable = false;
                        lastException = ex;
                        break;
                    }
                }

                if (allAvailable)
                    return;

                Thread.Sleep(100);
            }
            while (DateTime.UtcNow < deadline);

            throw new IOException("One or more Droute target files are still in use.", lastException);
        }
    }
}
