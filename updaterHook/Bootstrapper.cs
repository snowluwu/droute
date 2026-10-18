using Droute.Core;
using HarmonyLib;
using System;
using System.Diagnostics;
using System.IO;
using System.Reflection;

namespace Droute.UpdaterHook
{
    public class Bootstrapper : AppDomainManager
    {
        public override void InitializeNewDomain(AppDomainSetup appDomainInfo)
        {
            try
            {
                AppDomain.CurrentDomain.AssemblyLoad += OnAssemblyLoad;
                Logger.Info($"event=updater.domain.ready domain={AppDomain.CurrentDomain.Id} name=\"{AppDomain.CurrentDomain.FriendlyName}\" action=subscribe_assembly_load");
            }
            catch (Exception ex)
            {
                Logger.Error($"event=updater.domain.failed exception={ex}");
            }
        }

        private static void OnAssemblyLoad(object sender, AssemblyLoadEventArgs args)
        {
            try
            {
                if (args?.LoadedAssembly == null)
                    return;

                string name = args.LoadedAssembly.GetName()?.Name;
                if (string.IsNullOrEmpty(name))
                    return;

                if (name.Equals("Update.exe", StringComparison.OrdinalIgnoreCase))
                {
                    AppDomain.CurrentDomain.AssemblyLoad -= OnAssemblyLoad;
                    Logger.Info($"event=updater.assembly.loaded assembly=\"{args.LoadedAssembly.FullName}\" path=\"{args.LoadedAssembly.Location}\"");

                    Type type = args.LoadedAssembly.GetType("Squirrel.Update.Program");
                    if (type == null)
                    {
                        Logger.Error("type \"Squirrel.Update.Program\" not found in assembly");
                        return;
                    }

                    MethodInfo baseMethod = type.GetMethod("ProcessStart",
                        BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
                    if (baseMethod == null)
                    {
                        Logger.Error("target method \"ProcessStart\" not found");
                        return;
                    }

                    MethodInfo prefixMethod = typeof(Bootstrapper).GetMethod(nameof(BeforeProcessStart),
                        BindingFlags.Static | BindingFlags.Public);
                    if (prefixMethod == null)
                    {
                        Logger.Error("detour method \"BeforeProcessStart\" not found in hook assembly");
                        return;
                    }

                    Logger.Debug("applying harmony prefix patch for ProcessStart");

                    var harmony = new Harmony("Droute.UpdaterHook");
                    harmony.Patch(baseMethod, prefix: new HarmonyMethod(prefixMethod));

                    Logger.Info($"event=updater.hooks.ready target=\"{baseMethod.DeclaringType.FullName}.{baseMethod.Name}\" harmony_version={typeof(Harmony).Assembly.GetName().Version}");
                }
            }
            catch (Exception ex)
            {
                Logger.Error($"event=updater.hooks.failed exception={ex}");
            }
        }

        public static bool BeforeProcessStart(object __instance, string exeName, string arguments, bool shouldWait)
        {
            Logger.Debug($"event=updater.launch.request executable=\"{exeName}\" arguments_length={arguments?.Length ?? 0} wait={shouldWait}");

            string branchRoot = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);
            if (!Discord.IsTargetExecutable(branchRoot, exeName))
            {
                Logger.Debug($"skipping non-Discord process: {exeName}");
                return true;
            }

            string operation = Guid.NewGuid().ToString("N");
            var clock = Stopwatch.StartNew();
            string stage = "resolve_app_directory";
            string outcome = "failed";
            Logger.Info($"event=updater.patch.start operation={operation} root=\"{branchRoot}\" executable=\"{exeName}\"");

            try
            {
                string appDirectory = Discord.GetLatestAppDirectory(branchRoot);

                Logger.Info($"event=updater.patch.target operation={operation} directory=\"{appDirectory}\"");
                string proxyPath = PatchInstaller.GetProxyPath(appDirectory);
                string libraryPath = PatchInstaller.GetNativeLibraryPath(appDirectory);
                Logger.Info($"event=updater.patch.inspect operation={operation} proxy=\"{proxyPath}\" proxy_exists={File.Exists(proxyPath)} payload=\"{libraryPath}\" payload_exists={File.Exists(libraryPath)}");
                if (PatchInstaller.HasPatchFiles(appDirectory))
                {
                    outcome = "skipped_existing_files";
                    return true;
                }

                stage = "install";
                PatchInstaller.InstallPatch(appDirectory, Properties.Resources.Droute64, update =>
                {
                    if (update.Stage == InstallationStage.CleanupFailed)
                    {
                        Logger.Warning($"event=updater.patch.cleanup_failed operation={operation} path=\"{update.Path}\" exception={update.Error}");
                    }
                    else
                    {
                        if (update.Stage != InstallationStage.TemporaryFileDeleted)
                            stage = update.Stage.ToString();

                        Logger.Debug($"event=updater.patch.stage operation={operation} stage={update.Stage} path=\"{update.Path}\"");
                    }
                });

                outcome = "installed";
            }
            catch (Exception ex)
            {
                Logger.Error($"event=updater.patch.failed operation={operation} stage={stage} elapsed_ms={clock.ElapsedMilliseconds} exception={ex}");
            }
            finally
            {
                Logger.Info($"event=updater.patch.finish operation={operation} outcome={outcome} stage={stage} elapsed_ms={clock.ElapsedMilliseconds} launch_action=continue");
            }

            return true;
        }
    }
}
