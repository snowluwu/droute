using System;
using System.IO;

namespace Droute.Core
{
    public static class Droute
    {
        public const string UPDATER_HOOK_DLL = "Droute.UpdaterHook.dll";
        public const string UPDATER_CONFIG = "Update.exe.config";
        public const string UPDATER_EXE = "Update.exe";

        public static string GetProxyPath(string appDirectory)
            => Path.Combine(appDirectory, PatchManager.MAIN_PROXY_DLL);

        public static string GetPayloadPath(string appDirectory)
            => Path.Combine(appDirectory, PatchManager.MAIN_PAYLOAD_DLL);

        public static string GetUpdaterPath(string branchRoot)
            => Path.Combine(branchRoot, UPDATER_EXE);

        public static string GetUpdaterHookPath(string branchRoot)
            => Path.Combine(branchRoot, UPDATER_HOOK_DLL);

        public static string GetUpdaterConfigPath(string branchRoot)
            => Path.Combine(branchRoot, UPDATER_CONFIG);

        public static bool IsInstalled(string appDirectory)
        {
            if (string.IsNullOrEmpty(appDirectory))
                return false;

            return File.Exists(GetProxyPath(appDirectory)) &&
                   File.Exists(GetPayloadPath(appDirectory));
        }

        public static void Install(string appDirectory, byte[] payload)
            => Install(appDirectory, payload, null);

        public static void Install(string appDirectory, byte[] payload, Action<string, string> observe)
        {
            if (string.IsNullOrEmpty(appDirectory) || !Directory.Exists(appDirectory))
                throw new DirectoryNotFoundException("App directory not found.");

            if (payload == null || payload.Length == 0)
                throw new ArgumentException("Payload is required.", nameof(payload));

            string proxyPath = GetProxyPath(appDirectory);
            string payloadPath = GetPayloadPath(appDirectory);
            string stagingSuffix = CreateStagingSuffix();
            string proxyStagedPath = proxyPath + stagingSuffix;
            string payloadStagedPath = payloadPath + stagingSuffix;

            try
            {
                ObserveInstall(observe, "duplicate_proxy", $"staged=\"{proxyStagedPath}\"");
                PatchManager.DuplicateProxy(proxyStagedPath);
                ObserveInstall(observe, "patch_pe", $"staged=\"{proxyStagedPath}\"");
                PatchManager.ApplyPEPatch(proxyStagedPath);
                ObserveInstall(observe, "stage_payload", $"staged=\"{payloadStagedPath}\" bytes={payload.Length}");
                File.WriteAllBytes(payloadStagedPath, payload);

                ObserveInstall(observe, "publish_payload", $"destination=\"{payloadPath}\"");
                PatchManager.PublishStagedFile(payloadStagedPath, payloadPath);
                payloadStagedPath = null;

                ObserveInstall(observe, "publish_proxy", $"destination=\"{proxyPath}\"");
                PatchManager.PublishStagedFile(proxyStagedPath, proxyPath);
                proxyStagedPath = null;
            }
            finally
            {
                CleanupInstallFile(proxyStagedPath, observe);
                CleanupInstallFile(payloadStagedPath, observe);
            }
        }

        private static void ObserveInstall(Action<string, string> observe, string stage, string details)
        {
            // Diagnostics must not change installation or cleanup behavior.
            try { observe?.Invoke(stage, details); }
            catch { }
        }

        private static void CleanupInstallFile(string path, Action<string, string> observe)
        {
            if (string.IsNullOrEmpty(path)) return;
            try
            {
                if (!File.Exists(path)) return;
                File.Delete(path);
                ObserveInstall(observe, "cleanup", $"path=\"{path}\" outcome=deleted");
            }
            catch (Exception ex)
            {
                ObserveInstall(observe, "cleanup_failed", $"path=\"{path}\" exception={ex}");
            }
        }

        public static void Install(string appDirectory, string payloadPath)
        {
            if (string.IsNullOrEmpty(payloadPath))
                throw new ArgumentException("Payload path is required.", nameof(payloadPath));

            if (!File.Exists(payloadPath))
                throw new FileNotFoundException("Payload file not found.", payloadPath);

            Install(appDirectory, File.ReadAllBytes(payloadPath));
        }

        public static void Remove(string appDirectory)
        {
            if (string.IsNullOrEmpty(appDirectory) || !Directory.Exists(appDirectory))
                return;

            PatchManager.DeleteFile(GetProxyPath(appDirectory));
            PatchManager.DeleteFile(GetPayloadPath(appDirectory));
        }

        public static void InstallUpdaterHook(string branchRoot, byte[] updaterHook, string updaterConfig)
        {
            if (string.IsNullOrEmpty(branchRoot) || !Directory.Exists(branchRoot))
                throw new DirectoryNotFoundException("Branch root directory not found.");

            if (updaterHook == null || updaterHook.Length == 0)
                throw new ArgumentException("Updater hook is required.", nameof(updaterHook));

            if (string.IsNullOrEmpty(updaterConfig))
                throw new ArgumentException("Updater config is required.", nameof(updaterConfig));

            string updaterPath = GetUpdaterPath(branchRoot);
            if (!File.Exists(updaterPath))
                throw new FileNotFoundException("Update.exe not found.", updaterPath);

            string updaterHookPath = GetUpdaterHookPath(branchRoot);
            string updaterConfigPath = GetUpdaterConfigPath(branchRoot);
            string stagingSuffix = CreateStagingSuffix();
            string updaterHookStagedPath = updaterHookPath + stagingSuffix;
            string updaterConfigStagedPath = updaterConfigPath + stagingSuffix;

            try
            {
                File.WriteAllBytes(updaterHookStagedPath, updaterHook);
                File.WriteAllText(updaterConfigStagedPath, updaterConfig);

                PatchManager.PublishStagedFile(updaterHookStagedPath, updaterHookPath);
                updaterHookStagedPath = null;

                PatchManager.PublishStagedFile(updaterConfigStagedPath, updaterConfigPath);
                updaterConfigStagedPath = null;
            }
            finally
            {
                PatchManager.DeleteStagedFile(updaterHookStagedPath);
                PatchManager.DeleteStagedFile(updaterConfigStagedPath);
            }
        }

        public static void InstallUpdaterHook(string branchRoot, string updaterHookPath, string updaterConfigPath)
        {
            if (string.IsNullOrEmpty(updaterHookPath))
                throw new ArgumentException("Updater hook path is required.", nameof(updaterHookPath));

            if (string.IsNullOrEmpty(updaterConfigPath))
                throw new ArgumentException("Updater config path is required.", nameof(updaterConfigPath));

            if (!File.Exists(updaterHookPath))
                throw new FileNotFoundException("Updater hook file not found.", updaterHookPath);

            if (!File.Exists(updaterConfigPath))
                throw new FileNotFoundException("Updater config file not found.", updaterConfigPath);

            InstallUpdaterHook(branchRoot, File.ReadAllBytes(updaterHookPath), File.ReadAllText(updaterConfigPath));
        }

        public static void RemoveUpdaterHook(string branchRoot)
        {
            if (string.IsNullOrEmpty(branchRoot) || !Directory.Exists(branchRoot))
                return;

            PatchManager.DeleteFile(GetUpdaterHookPath(branchRoot));
            PatchManager.DeleteFile(GetUpdaterConfigPath(branchRoot));
        }

        public static void Install(DiscordManager.Branches branch, byte[] payload, byte[] updaterHook, string updaterConfig)
        {
            string branchRoot = DiscordManager.GetBranchRoot(branch);
            if (string.IsNullOrEmpty(branchRoot) || !Directory.Exists(branchRoot))
                throw new DirectoryNotFoundException("Branch root directory not found.");

            string appDirectory = DiscordManager.GetLastVersionPath(branchRoot);
            if (string.IsNullOrEmpty(appDirectory) || !Directory.Exists(appDirectory))
                throw new DirectoryNotFoundException("App directory not found.");

            string updaterPath = GetUpdaterPath(branchRoot);
            if (!File.Exists(updaterPath))
                throw new FileNotFoundException("Update.exe not found.", updaterPath);

            PatchManager.WaitForFilesAvailable(new[]
            {
                GetProxyPath(appDirectory),
                GetPayloadPath(appDirectory),
                GetUpdaterHookPath(branchRoot),
                GetUpdaterConfigPath(branchRoot)
            }, 5000);

            Install(appDirectory, payload);
            InstallUpdaterHook(branchRoot, updaterHook, updaterConfig);
        }

        public static void Install(DiscordManager.Branches branch, string payloadPath, string updaterHookPath, string updaterConfigPath)
        {
            if (string.IsNullOrEmpty(payloadPath))
                throw new ArgumentException("Payload path is required.", nameof(payloadPath));

            if (string.IsNullOrEmpty(updaterHookPath))
                throw new ArgumentException("Updater hook path is required.", nameof(updaterHookPath));

            if (string.IsNullOrEmpty(updaterConfigPath))
                throw new ArgumentException("Updater config path is required.", nameof(updaterConfigPath));

            if (!File.Exists(payloadPath))
                throw new FileNotFoundException("Payload file not found.", payloadPath);

            if (!File.Exists(updaterHookPath))
                throw new FileNotFoundException("Updater hook file not found.", updaterHookPath);

            if (!File.Exists(updaterConfigPath))
                throw new FileNotFoundException("Updater config file not found.", updaterConfigPath);

            Install(
                branch,
                File.ReadAllBytes(payloadPath),
                File.ReadAllBytes(updaterHookPath),
                File.ReadAllText(updaterConfigPath));
        }

        public static void Remove(DiscordManager.Branches branch)
        {
            string branchRoot = DiscordManager.GetBranchRoot(branch);

            if (string.IsNullOrEmpty(branchRoot) || !Directory.Exists(branchRoot))
                return;

            RemoveUpdaterHook(branchRoot);

            try { Remove(DiscordManager.GetLastVersionPath(branchRoot)); }
            catch { }
        }

        public static bool IsTargetExecutable(string branchRoot, string exeName)
        {
            if (string.IsNullOrEmpty(branchRoot) || string.IsNullOrWhiteSpace(exeName))
                return false;

            string branchName = Path.GetFileName(branchRoot.TrimEnd(
                Path.DirectorySeparatorChar,
                Path.AltDirectorySeparatorChar));

            string expectedExecutable;
            if (branchName.Equals("Discord", StringComparison.OrdinalIgnoreCase))
                expectedExecutable = "Discord.exe";
            else if (branchName.Equals("DiscordCanary", StringComparison.OrdinalIgnoreCase))
                expectedExecutable = "DiscordCanary.exe";
            else if (branchName.Equals("DiscordPTB", StringComparison.OrdinalIgnoreCase))
                expectedExecutable = "DiscordPTB.exe";
            else
                return false;

            string executableName = Path.GetFileName(exeName.Trim().Trim('"'));
            return expectedExecutable.Equals(executableName, StringComparison.OrdinalIgnoreCase);
        }

        private static string CreateStagingSuffix()
            => $".droute.{Guid.NewGuid():N}.tmp";
    }
}
