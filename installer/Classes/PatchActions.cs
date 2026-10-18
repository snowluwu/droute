using Droute.Core;
using System;
using System.IO;

namespace Droute.Installer.Classes
{
    internal enum MessageLevel
    {
        Info,
        Stage,
        Success,
        Warning,
        Error,
        Step,
        Removal
    }

    internal static class PatchActions
    {
        public static bool Install(DiscordBranch branch, Action<MessageLevel, string> log, Action<int> progress = null)
            => Execute(branch, true, log, progress);

        public static bool Uninstall(DiscordBranch branch, Action<MessageLevel, string> log, Action<int> progress = null)
            => Execute(branch, false, log, progress);

        private static bool Execute(DiscordBranch branch, bool install,
            Action<MessageLevel, string> log, Action<int> progress)
        {
            try
            {
                log(MessageLevel.Info, install
                    ? $"Installing Droute for Discord {branch}."
                    : $"Removing Droute from Discord {branch}.");
                if (!install)
                    log(MessageLevel.Stage, "Removing updater hook...");
                progress?.Invoke(0);

                Action<InstallationProgress> onProgress = update =>
                {
                    switch (update.Stage)
                    {
                        case InstallationStage.CopyingProxy:
                            log(MessageLevel.Stage, "Installing Discord patch...");
                            progress?.Invoke(15);
                            break;

                        case InstallationStage.PatchingProxy:
                            progress?.Invoke(30);
                            break;

                        case InstallationStage.StagingLibrary:
                            progress?.Invoke(40);
                            break;

                        case InstallationStage.PublishingLibrary:
                            log(MessageLevel.Step, $"Installing {PatchInstaller.NativeLibraryFileName}...");
                            progress?.Invoke(50);
                            break;

                        case InstallationStage.PublishingProxy:
                            log(MessageLevel.Step, $"Installing {PatchInstaller.ProxyFileName}...");
                            progress?.Invoke(60);
                            break;

                        case InstallationStage.StagingUpdaterHook:
                            log(MessageLevel.Stage, "Enabling Droute after Discord updates...");
                            progress?.Invoke(65);
                            break;

                        case InstallationStage.StagingUpdaterConfig:
                            progress?.Invoke(75);
                            break;

                        case InstallationStage.PublishingUpdaterHook:
                            log(MessageLevel.Step, $"Installing {PatchInstaller.UpdaterHookFileName}...");
                            progress?.Invoke(80);
                            break;

                        case InstallationStage.PublishingUpdaterConfig:
                            log(MessageLevel.Step, $"Installing {PatchInstaller.UpdaterConfigFileName}...");
                            progress?.Invoke(90);
                            break;

                        case InstallationStage.UpdaterHookInstalled:
                            progress?.Invoke(95);
                            break;

                        case InstallationStage.RemovingFile:
                            if (File.Exists(update.Path))
                                log(MessageLevel.Removal, $"Removing {Path.GetFileName(update.Path)}...");
                            break;

                        case InstallationStage.UpdaterHookUninstalled:
                            progress?.Invoke(50);
                            log(MessageLevel.Stage, "Removing Discord patch...");
                            break;

                        case InstallationStage.PatchUninstalled:
                            progress?.Invoke(95);
                            break;

                        case InstallationStage.CleanupFailed:
                            log(MessageLevel.Warning, $"Unable to remove temporary file {update.Path}: {update.Error.Message}");
                            break;
                    }
                };

                if (install)
                {
                    var files = InstallationFiles.FromBytes(Properties.Resources.Droute64,
                        Properties.Resources.UpdaterHook, Properties.Resources.UpdaterConfig);

                    PatchInstaller.Install(branch, files, onProgress);
                }
                else
                {
                    PatchInstaller.Uninstall(branch, onProgress);
                }

                progress?.Invoke(100);
                log(MessageLevel.Success, install
                    ? $"Droute installed for Discord {branch}."
                    : $"Droute removed from Discord {branch}.");
                return true;
            }
            catch (Exception ex)
            {
                log(MessageLevel.Error, ex.Message);
                return false;
            }
        }
    }
}
