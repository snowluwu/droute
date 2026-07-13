using CommandLine;
using Droute.Core;
using Droute.Installer.Classes;
using Droute.Installer.Forms;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;

namespace Droute.Installer
{
    internal static class Program
    {
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool AttachConsole(int dwProcessId);

        private static bool createdNew;
        private static Mutex mtx;

        [STAThread]
        static void Main(string[] args)
        {
            mtx = new Mutex(true, "snowluwu.droute", out createdNew);

            if (!createdNew)
                return;

            if (args.Length > 0)
            {
                if (AttachConsole(-1)) // ATTACH_PARENT_PROCESS
                {
                    Console.Out.Flush();
                }

                #region [ ASCII ART ]

                Console.ForegroundColor = ConsoleColor.Blue;
                Console.WriteLine(@"
    ____  ____  ____  __  ______________
   / __ \/ __ \/ __ \/ / / /_  __/ ____/
  / / / / /_/ / / / / / / / / / / __/   
 / /_/ / _, _/ /_/ / /_/ / / / / /___   
/_____/_/ |_|\____/\____/ /_/ /_____/

");

                #endregion

                #region [ About Droute ]

                var versionInfo = new Version(Application.ProductVersion);
                Console.ForegroundColor = ConsoleColor.DarkYellow;
                Console.WriteLine($"> CLI Mode (v. {versionInfo.Major}.{versionInfo.Minor}.{versionInfo.Build})");

                Console.ForegroundColor = ConsoleColor.White;
                Console.WriteLine("> by snowluwu <3");
                Console.WriteLine();

                #endregion

                Parser.Default.ParseArguments<ArgumentOptions>(NormalizeCliArguments(args))
                    .WithParsed(CliActions)
                    .WithNotParsed(OnCliError);

                Console.ResetColor();
                SendKeys.SendWait("{ENTER}");
                return;
            }

            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new FrmMain());
        }

        private static void CliActions(ArgumentOptions opts)
        {
            if (opts.Install && opts.Uninstall)
            {
                CliLogger.WriteError("Use either `-install` or `-uninstall`, not both.");
                Environment.ExitCode = 2;
                return;
            }

            if (!Enum.TryParse(opts.Branch, true, out DiscordManager.Branches branch) ||
                !Enum.IsDefined(typeof(DiscordManager.Branches), branch))
            {
                CliLogger.WriteError("Invalid branch. Expected `stable`, `canary` or `ptb`.");
                Environment.ExitCode = 2;
                return;
            }

            using (var logger = new CliLogger())
            {
                try
                {
                    bool configChanged = ApplyOptionalConfig(opts);

                    if (!opts.Install && !opts.Uninstall)
                    {
                        if (!configChanged)
                        {
                            CliLogger.WriteError("No action or proxy settings were specified.");
                            Environment.ExitCode = 2;
                        }
                        else
                        {
                            Environment.ExitCode = 0;
                        }

                        return;
                    }

                    bool success = opts.Install
                        ? PatchTools.Install(branch)
                        : PatchTools.Remove(branch);

                    Environment.ExitCode = success ? 0 : 1;
                }
                catch (Exception ex)
                {
                    CliLogger.WriteError(ex.Message);
                    Environment.ExitCode = 1;
                }
            }
        }

        private static void OnCliError(IEnumerable<Error> errors)
        {
            bool helpRequested = errors.Any(error =>
                error is HelpRequestedError || error is VersionRequestedError);

            Environment.ExitCode = helpRequested ? 0 : 2;
        }

        private static string[] NormalizeCliArguments(string[] args)
        {
            var normalized = new string[args.Length];

            for (int i = 0; i < args.Length; i++)
            {
                if (string.Equals(args[i], "-install", StringComparison.OrdinalIgnoreCase))
                    normalized[i] = "--install";
                else if (string.Equals(args[i], "-uninstall", StringComparison.OrdinalIgnoreCase))
                    normalized[i] = "--uninstall";
                else
                    normalized[i] = args[i];
            }

            return normalized;
        }

        private static bool ApplyOptionalConfig(ArgumentOptions opts)
        {
            bool changed = false;
            var config = new Config();

            if (opts.Host != null)
            {
                config.Host = opts.Host;
                changed = true;
            }

            if (opts.Port.HasValue)
            {
                config.Port = opts.Port.Value;
                changed = true;
            }

            if (opts.User != null)
            {
                config.User = opts.User;
                changed = true;
            }

            if (opts.Password != null)
            {
                config.Password = opts.Password;
                changed = true;
            }

            if (!changed)
                return false;

            config.Apply();

            CliLogger.WriteOk("Proxy configuration updated.");
            return true;
        }
    }
}
