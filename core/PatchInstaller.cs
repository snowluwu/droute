using System;
using System.Diagnostics;
using System.IO;

namespace Droute.Core
{
    /// <summary>installs and removes the native patch and updater hook independently or together.</summary>
    public static class PatchInstaller
    {
        public const string ProxyFileName = "version.dll";
        public const string NativeLibraryFileName = "droute.dll";
        public const string UpdaterHookFileName = "Droute.UpdaterHook.dll";
        public const string UpdaterConfigFileName = "Update.exe.config";
        public const string UpdaterFileName = "Update.exe";

        public static string GetProxyPath(string appDirectory)
            => Path.Combine(appDirectory, ProxyFileName);

        public static string GetNativeLibraryPath(string appDirectory)
            => Path.Combine(appDirectory, NativeLibraryFileName);

        public static string GetUpdaterPath(string rootDirectory)
            => Path.Combine(rootDirectory, UpdaterFileName);

        public static string GetUpdaterHookPath(string rootDirectory)
            => Path.Combine(rootDirectory, UpdaterHookFileName);

        public static string GetUpdaterConfigPath(string rootDirectory)
            => Path.Combine(rootDirectory, UpdaterConfigFileName);

        /// <summary>checks for both patch files; does not verify their contents or runtime state.</summary>
        public static bool HasPatchFiles(string appDirectory)
            => !string.IsNullOrEmpty(appDirectory) &&
                File.Exists(GetProxyPath(appDirectory)) && File.Exists(GetNativeLibraryPath(appDirectory));

        /// <summary>installs both components for the latest version of a standard discord branch.</summary>
        public static void Install(DiscordBranch branch, InstallationFiles files,
            Action<InstallationProgress> onProgress = null)
            => Install(Discord.GetInstallation(branch), files, onProgress);

        /// <summary>installs both components; the caller must close discord first.</summary>
        /// <exception cref="IOException">a target file could not be written; earlier steps may have completed.</exception>
        public static void Install(DiscordInstallation installation, InstallationFiles files,
            Action<InstallationProgress> onProgress = null)
        {
            if (installation == null)
                throw new ArgumentNullException(nameof(installation));

            if (files == null)
                throw new ArgumentNullException(nameof(files));

            string updaterPath = GetUpdaterPath(installation.RootDirectory);
            if (!File.Exists(updaterPath))
                throw new FileNotFoundException("Update.exe not found.", updaterPath);

            InstallPatch(installation.AppDirectory, files.NativeLibrary, onProgress);
            InstallUpdaterHook(installation.RootDirectory, files.UpdaterHook, files.UpdaterConfig, onProgress);
        }

        /// <summary>installs only the native patch in an existing app directory; close discord first.</summary>
        /// <exception cref="DirectoryNotFoundException">the app directory does not exist.</exception>
        /// <exception cref="IOException">a patch file could not be prepared or published.</exception>
        public static void InstallPatch(string appDirectory, byte[] nativeLibrary,
            Action<InstallationProgress> onProgress = null)
        {
            if (string.IsNullOrWhiteSpace(appDirectory) || !Directory.Exists(appDirectory))
                throw new DirectoryNotFoundException("App directory not found.");

            if (nativeLibrary == null || nativeLibrary.Length == 0)
                throw new ArgumentException("Native library is required.", nameof(nativeLibrary));

            string proxyPath = GetProxyPath(appDirectory);
            string libraryPath = GetNativeLibraryPath(appDirectory);
            string suffix = CreateStagingSuffix();
            string stagedProxy = proxyPath + suffix;
            string stagedLibrary = libraryPath + suffix;

            try
            {
                Report(onProgress, InstallationStage.CopyingProxy, stagedProxy);
                PatchFiles.CopySystemProxy(stagedProxy);

                Report(onProgress, InstallationStage.PatchingProxy, stagedProxy);
                PatchFiles.AddLibraryImport(stagedProxy);

                Report(onProgress, InstallationStage.StagingLibrary, stagedLibrary);
                File.WriteAllBytes(stagedLibrary, nativeLibrary);

                Report(onProgress, InstallationStage.PublishingLibrary, libraryPath);
                PatchFiles.PublishStagedFile(stagedLibrary, libraryPath);
                stagedLibrary = null;

                Report(onProgress, InstallationStage.PublishingProxy, proxyPath);
                PatchFiles.PublishStagedFile(stagedProxy, proxyPath);
                stagedProxy = null;

                Report(onProgress, InstallationStage.PatchInstalled, appDirectory);
            }
            finally
            {
                Cleanup(stagedProxy, onProgress);
                Cleanup(stagedLibrary, onProgress);
            }
        }

        /// <summary>reads the native library from a file and installs only the app patch.</summary>
        public static void InstallPatch(string appDirectory, string nativeLibraryPath,
            Action<InstallationProgress> onProgress = null)
            => InstallPatch(appDirectory, File.ReadAllBytes(nativeLibraryPath), onProgress);

        /// <summary>installs only the updater hook beside Update.exe.</summary>
        /// <exception cref="IOException">the updater is missing or a hook file could not be written.</exception>
        public static void InstallUpdaterHook(string rootDirectory, byte[] updaterHook, string updaterConfig,
            Action<InstallationProgress> onProgress = null)
        {
            if (string.IsNullOrWhiteSpace(rootDirectory) || !Directory.Exists(rootDirectory))
                throw new DirectoryNotFoundException("Root directory not found.");

            if (updaterHook == null || updaterHook.Length == 0)
                throw new ArgumentException("Updater hook is required.", nameof(updaterHook));

            if (string.IsNullOrWhiteSpace(updaterConfig))
                throw new ArgumentException("Updater config is required.", nameof(updaterConfig));

            string updaterPath = GetUpdaterPath(rootDirectory);
            if (!File.Exists(updaterPath))
                throw new FileNotFoundException("Update.exe not found.", updaterPath);

            string hookPath = GetUpdaterHookPath(rootDirectory);
            string configPath = GetUpdaterConfigPath(rootDirectory);
            string suffix = CreateStagingSuffix();
            string stagedHook = hookPath + suffix;
            string stagedConfig = configPath + suffix;

            try
            {
                Report(onProgress, InstallationStage.StagingUpdaterHook, stagedHook);
                File.WriteAllBytes(stagedHook, updaterHook);

                Report(onProgress, InstallationStage.StagingUpdaterConfig, stagedConfig);
                File.WriteAllText(stagedConfig, updaterConfig);

                Report(onProgress, InstallationStage.PublishingUpdaterHook, hookPath);
                PatchFiles.PublishStagedFile(stagedHook, hookPath);
                stagedHook = null;

                Report(onProgress, InstallationStage.PublishingUpdaterConfig, configPath);
                PatchFiles.PublishStagedFile(stagedConfig, configPath);
                stagedConfig = null;

                Report(onProgress, InstallationStage.UpdaterHookInstalled, rootDirectory);
            }
            finally
            {
                Cleanup(stagedHook, onProgress);
                Cleanup(stagedConfig, onProgress);
            }
        }

        /// <summary>reads the hook and config files before installing the updater hook.</summary>
        public static void InstallUpdaterHook(string rootDirectory, string updaterHookPath, string updaterConfigPath,
            Action<InstallationProgress> onProgress = null)
            => InstallUpdaterHook(rootDirectory, File.ReadAllBytes(updaterHookPath),
                File.ReadAllText(updaterConfigPath), onProgress);

        /// <summary>removes both components from a standard branch; missing files are ignored.</summary>
        public static void Uninstall(DiscordBranch branch, Action<InstallationProgress> onProgress = null)
        {
            string root = Discord.GetRootDirectory(branch);
            UninstallUpdaterHook(root, onProgress);

            string appDirectory;
            try
            {
                appDirectory = Discord.GetLatestAppDirectory(root);
            }
            catch (DirectoryNotFoundException)
            {
                return;
            }

            UninstallPatch(appDirectory, onProgress);
        }

        /// <summary>removes both components from the resolved installation; close discord first.</summary>
        /// <exception cref="IOException">a target file could not be deleted.</exception>
        public static void Uninstall(DiscordInstallation installation, Action<InstallationProgress> onProgress = null)
        {
            if (installation == null)
                throw new ArgumentNullException(nameof(installation));

            UninstallUpdaterHook(installation.RootDirectory, onProgress);
            UninstallPatch(installation.AppDirectory, onProgress);
        }

        /// <summary>removes only the app patch; a missing app directory or patch is ignored.</summary>
        public static void UninstallPatch(string appDirectory, Action<InstallationProgress> onProgress = null)
        {
            if (string.IsNullOrWhiteSpace(appDirectory))
                throw new ArgumentException("App directory is required.", nameof(appDirectory));

            if (!Directory.Exists(appDirectory))
                return;

            string proxyPath = GetProxyPath(appDirectory);
            string libraryPath = GetNativeLibraryPath(appDirectory);
            Report(onProgress, InstallationStage.RemovingFile, proxyPath);
            File.Delete(proxyPath);
            Report(onProgress, InstallationStage.RemovingFile, libraryPath);
            File.Delete(libraryPath);

            Report(onProgress, InstallationStage.PatchUninstalled, appDirectory);
        }

        /// <summary>removes only the updater hook and config; missing files are ignored.</summary>
        public static void UninstallUpdaterHook(string rootDirectory, Action<InstallationProgress> onProgress = null)
        {
            if (string.IsNullOrWhiteSpace(rootDirectory))
                throw new ArgumentException("Root directory is required.", nameof(rootDirectory));

            if (!Directory.Exists(rootDirectory))
                return;

            string hookPath = GetUpdaterHookPath(rootDirectory);
            string configPath = GetUpdaterConfigPath(rootDirectory);
            Report(onProgress, InstallationStage.RemovingFile, hookPath);
            File.Delete(hookPath);
            Report(onProgress, InstallationStage.RemovingFile, configPath);
            File.Delete(configPath);

            Report(onProgress, InstallationStage.UpdaterHookUninstalled, rootDirectory);
        }

        private static void Report(Action<InstallationProgress> onProgress, InstallationStage stage,
            string path, Exception error = null)
        {
            if (error != null)
                Trace.WriteLine(error);

            try
            {
                onProgress?.Invoke(new InstallationProgress(stage, path, error));
            }
            catch (Exception ex)
            {
                // progress callbacks must not interrupt file cleanup
                Trace.WriteLine(ex);
            }
        }

        private static void Cleanup(string path, Action<InstallationProgress> onProgress)
        {
            if (path == null || !File.Exists(path))
                return;

            try
            {
                File.Delete(path);
                Report(onProgress, InstallationStage.TemporaryFileDeleted, path);
            }
            catch (Exception ex)
            {
                Report(onProgress, InstallationStage.CleanupFailed, path, ex);
            }
        }

        private static string CreateStagingSuffix()
            => $".droute.{Guid.NewGuid():N}.tmp";
    }
}
