using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;

namespace Droute.Core
{
    /// <summary>installs and removes the native patch and updater hook independently or together.</summary>
    public static class PatchInstaller
    {
        public const string ProxyFileName = "version.dll";
        public const string NativeLibraryFileName = "droute.dll";
        public const string WeaveLibraryFileName = "weave_proxy.dll";
        public const string UpdaterHookFileName = "Droute.UpdaterHook.dll";
        public const string UpdaterConfigFileName = "Update.exe.config";
        public const string UpdaterFileName = "Update.exe";

        public static string GetProxyPath(string appDirectory)
            => Path.Combine(appDirectory, ProxyFileName);

        public static string GetNativeLibraryPath(string appDirectory)
            => Path.Combine(appDirectory, NativeLibraryFileName);

        public static string GetWeaveLibraryPath(string appDirectory)
            => Path.Combine(appDirectory, WeaveLibraryFileName);

        public static string GetUpdaterPath(string rootDirectory)
            => Path.Combine(rootDirectory, UpdaterFileName);

        public static string GetUpdaterHookPath(string rootDirectory)
            => Path.Combine(rootDirectory, UpdaterHookFileName);

        public static string GetUpdaterConfigPath(string rootDirectory)
            => Path.Combine(rootDirectory, UpdaterConfigFileName);

        /// <summary>checks for all three patch files; does not verify their contents or runtime state.</summary>
        public static bool HasPatchFiles(string appDirectory)
            => !string.IsNullOrEmpty(appDirectory) &&
                File.Exists(GetProxyPath(appDirectory)) && File.Exists(GetNativeLibraryPath(appDirectory)) &&
                File.Exists(GetWeaveLibraryPath(appDirectory));

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

            InstallPatch(installation.AppDirectory, files.NativeLibrary, files.WeaveLibrary, onProgress);
            InstallUpdaterHook(installation.RootDirectory, files.UpdaterHook, files.UpdaterConfig, onProgress);
        }

        /// <summary>installs only the native patch in an existing app directory; close discord first.</summary>
        /// <exception cref="DirectoryNotFoundException">the app directory does not exist.</exception>
        /// <exception cref="IOException">a patch file could not be prepared or published.</exception>
        public static void InstallPatch(string appDirectory, byte[] nativeLibrary, byte[] weaveLibrary,
            Action<InstallationProgress> onProgress = null)
        {
            if (string.IsNullOrWhiteSpace(appDirectory) || !Directory.Exists(appDirectory))
                throw new DirectoryNotFoundException("App directory not found.");

            if (nativeLibrary == null || nativeLibrary.Length == 0)
                throw new ArgumentException("Native library is required.", nameof(nativeLibrary));

            if (weaveLibrary == null || weaveLibrary.Length == 0)
                throw new ArgumentException("Weave library is required.", nameof(weaveLibrary));

            string[] targets =
            {
                GetWeaveLibraryPath(appDirectory), GetNativeLibraryPath(appDirectory), GetProxyPath(appDirectory)
            };
            string suffix = CreateStagingSuffix();
            string[] staged = Array.ConvertAll(targets, path => path + suffix);
            string[] backups = Array.ConvertAll(targets, path => path + suffix + ".bak");
            bool[] existed = Array.ConvertAll(targets, File.Exists);
            bool[] published = new bool[targets.Length];
            bool[] keepBackup = new bool[targets.Length];

            try
            {
                Report(onProgress, InstallationStage.CopyingProxy, staged[2]);
                PatchFiles.CopySystemProxy(staged[2]);
                Report(onProgress, InstallationStage.PatchingProxy, staged[2]);
                PatchFiles.AddLibraryImport(staged[2]);

                Report(onProgress, InstallationStage.StagingLibrary, staged[0]);
                File.WriteAllBytes(staged[0], weaveLibrary);
                Report(onProgress, InstallationStage.StagingLibrary, staged[1]);
                File.WriteAllBytes(staged[1], nativeLibrary);

                for (int i = 0; i < targets.Length; ++i)
                    if (existed[i])
                        File.Copy(targets[i], backups[i]);

                // publish dependencies before the proxy can load them
                for (int i = 0; i < targets.Length; ++i)
                {
                    var stage = i == 2 ? InstallationStage.PublishingProxy : InstallationStage.PublishingLibrary;
                    Report(onProgress, stage, targets[i]);
                    PatchFiles.PublishStagedFile(staged[i], targets[i]);
                    published[i] = true;
                }
                Report(onProgress, InstallationStage.PatchInstalled, appDirectory);
            }
            catch (Exception error)
            {
                var errors = new List<Exception> { error };
                for (int i = targets.Length - 1; i >= 0; --i)
                {
                    if (!published[i])
                        continue;

                    try
                    {
                        if (existed[i])
                            PatchFiles.PublishStagedFile(backups[i], targets[i]);
                        else
                            File.Delete(targets[i]);
                    }
                    catch (Exception rollbackError)
                    {
                        keepBackup[i] = true;
                        errors.Add(new IOException($"Could not restore {targets[i]}. Backup: {backups[i]}", rollbackError));
                    }
                }
                if (errors.Count > 1)
                    throw new IOException("Patch installation and rollback failed.\n" +
                        string.Join("\n", errors.ConvertAll(item => item.ToString())), error);
                throw;
            }
            finally
            {
                for (int i = 0; i < targets.Length; ++i)
                {
                    Cleanup(staged[i], onProgress);
                    if (!keepBackup[i])
                        Cleanup(backups[i], onProgress);
                }
            }
        }

        /// <summary>reads both libraries before installing the app patch.</summary>
        public static void InstallPatch(string appDirectory, string nativeLibraryPath, string weaveLibraryPath,
            Action<InstallationProgress> onProgress = null)
            => InstallPatch(appDirectory, File.ReadAllBytes(nativeLibraryPath), File.ReadAllBytes(weaveLibraryPath), onProgress);

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
            string weavePath = GetWeaveLibraryPath(appDirectory);

            Report(onProgress, InstallationStage.RemovingFile, proxyPath);
            File.Delete(proxyPath);
            Report(onProgress, InstallationStage.RemovingFile, libraryPath);
            File.Delete(libraryPath);
            Report(onProgress, InstallationStage.RemovingFile, weavePath);
            File.Delete(weavePath);

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
