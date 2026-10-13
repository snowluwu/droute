using Droute.Core;
using System;
using System.IO;
using DrCore = Droute.Core.Droute;

namespace Droute.Installer.Classes
{
    internal class PatchTools
    {
        public static event Action<string> OnLog;
        public static event Action<int> OnProgressChanged;

        public static bool Install(DiscordManager.Branches branch)
        {
            try
            {
                OnLog?.Invoke($"[ STATUS ] Installation started for Discord {branch.ToString()}...");
                OnProgressChanged?.Invoke(0);

                OnLog?.Invoke("[ STAGE ] Initializing and verifying paths...");

                string branchRoot = DiscordManager.GetBranchRoot(branch);
                if (string.IsNullOrEmpty(branchRoot) || !Directory.Exists(branchRoot))
                    throw new DirectoryNotFoundException("Branch Root directory not found");

                string appDirectory = DiscordManager.GetLastVersionPath(branchRoot);
                if (string.IsNullOrEmpty(appDirectory) || !Directory.Exists(appDirectory))
                    throw new DirectoryNotFoundException("App directory not found");

                string updaterPath = DrCore.GetUpdaterPath(branchRoot);
                if (string.IsNullOrEmpty(updaterPath) || !File.Exists(updaterPath))
                    throw new FileNotFoundException("Update.exe not found");

                PatchManager.WaitForFilesAvailable(new[]
                {
                    DrCore.GetProxyPath(appDirectory),
                    DrCore.GetPayloadPath(appDirectory),
                    DrCore.GetUpdaterHookPath(branchRoot),
                    DrCore.GetUpdaterConfigPath(branchRoot)
                }, 5000);

                OnLog?.Invoke("[ OK ] All target environment paths verified successfully!");
                OnProgressChanged?.Invoke(15);

                OnLog?.Invoke("[ STAGE ] Deploying...");
                OnLog?.Invoke("[ STAGE ] Publishing prepared files...");

                OnLog?.Invoke($"[ > ] Preparing system proxy: {PatchManager.MAIN_PROXY_DLL}...");
                OnProgressChanged?.Invoke(30);

                OnLog?.Invoke($"[ > ] Applying Import Tables to: {PatchManager.MAIN_PROXY_DLL}...");
                OnProgressChanged?.Invoke(45);

                OnLog?.Invoke($"[ > ] Preparing payload: {PatchManager.MAIN_PAYLOAD_DLL}...");
                DrCore.Install(appDirectory, Properties.Resources.Droute64);
                OnProgressChanged?.Invoke(60);

                OnLog?.Invoke("[ STAGE ] Configuring Squirrel Update hooks...");

                OnLog?.Invoke($"[ > ] Preparing configuration: {DrCore.UPDATER_CONFIG}...");
                OnProgressChanged?.Invoke(75);

                OnLog?.Invoke($"[ > ] Preparing UpdaterHook: {DrCore.UPDATER_HOOK_DLL}...");
                DrCore.InstallUpdaterHook(branchRoot, Properties.Resources.UpdaterHook, Properties.Resources.UpdaterConfig);
                OnProgressChanged?.Invoke(90);

                OnLog?.Invoke("[ STATUS ] Installation successfully completed!");
                OnProgressChanged?.Invoke(100);

                return true;
            }
            catch (Exception ex)
            {
                OnLog?.Invoke($"[ FATAL ]: {ex.Message}");
                OnProgressChanged?.Invoke(100);
                return false;
            }
        }

        public static bool Remove(DiscordManager.Branches branch)
        {
            try
            {
                OnLog?.Invoke($"[ STATUS ] Removal started for Discord {branch.ToString()}...");
                OnProgressChanged?.Invoke(0);

                string branchRoot = DiscordManager.GetBranchRoot(branch);
                if (string.IsNullOrEmpty(branchRoot) || !Directory.Exists(branchRoot))
                {
                    OnLog?.Invoke("[ WARN ] Branch root directory not found. Nothing to remove.");
                    OnProgressChanged?.Invoke(100);
                    return true;
                }

                OnLog?.Invoke("[ STAGE ] Cleaning up Squirrel Update hooks...");

                if (File.Exists(DrCore.GetUpdaterHookPath(branchRoot)))
                    OnLog?.Invoke($"[ < ] Removing: {DrCore.UPDATER_HOOK_DLL}...");
                OnProgressChanged?.Invoke(30);

                if (File.Exists(DrCore.GetUpdaterConfigPath(branchRoot)))
                    OnLog?.Invoke($"[ < ] Removing: {DrCore.UPDATER_CONFIG}...");

                DrCore.RemoveUpdaterHook(branchRoot);
                OnProgressChanged?.Invoke(50);

                try
                {
                    string appDirectory = DiscordManager.GetLastVersionPath(branchRoot);

                    if (!string.IsNullOrEmpty(appDirectory) && Directory.Exists(appDirectory))
                    {
                        OnLog?.Invoke("[ STAGE ] Cleaning up...");

                        if (File.Exists(DrCore.GetProxyPath(appDirectory)))
                            OnLog?.Invoke($"[ < ] Deleting modified proxy: {PatchManager.MAIN_PROXY_DLL}...");
                        OnProgressChanged?.Invoke(75);

                        if (File.Exists(DrCore.GetPayloadPath(appDirectory)))
                            OnLog?.Invoke($"[ < ] Deleting payload: {PatchManager.MAIN_PAYLOAD_DLL}...");

                        DrCore.Remove(appDirectory);
                    }
                    else
                    {
                        OnLog?.Invoke("[ INFO ] Application version directory not found, skipping core payload cleanup.");
                    }
                }
                catch (DirectoryNotFoundException)
                {
                    OnLog?.Invoke("[ INFO ] Application version directory not found, skipping core payload cleanup.");
                }

                OnLog?.Invoke($"[ STATUS ] Removal completed successfully for Discord {branch}!");
                OnProgressChanged?.Invoke(100);

                return true;
            }
            catch (Exception ex)
            {
                OnLog?.Invoke($"[ FATAL ] Removal failed: {ex.Message}");
                return false;
            }
        }
    }
}
