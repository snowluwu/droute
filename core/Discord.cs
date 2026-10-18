using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Threading;

namespace Droute.Core
{
    public static class Discord
    {
        public static List<DiscordBranch> GetInstalledBranches()
        {
            var result = new List<DiscordBranch>();

            foreach (DiscordBranch branch in Enum.GetValues(typeof(DiscordBranch)))
            {
                if (Directory.Exists(GetRootDirectory(branch)))
                    result.Add(branch);
            }

            return result;
        }

        public static string GetRootDirectory(DiscordBranch branch)
        {
            return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                GetProcessName(branch));
        }

        public static string GetLatestAppDirectory(string branchRoot)
        {
            if (string.IsNullOrWhiteSpace(branchRoot))
                throw new ArgumentException("Root directory is required.", nameof(branchRoot));

            if (!Directory.Exists(branchRoot))
                throw new DirectoryNotFoundException($"Discord root directory not found: {branchRoot}");

            string latestPath = null;
            Version latestVersion = null;

            foreach (string path in Directory.EnumerateDirectories(branchRoot, "app-*"))
            {
                if (Version.TryParse(Path.GetFileName(path).Substring(4), out Version version) &&
                    (latestVersion == null || version > latestVersion))
                {
                    latestVersion = version;
                    latestPath = path;
                }
            }

            if (latestPath == null)
                throw new DirectoryNotFoundException("No valid version directories found in the branch root!");

            return latestPath;
        }

        public static string GetExecutablePath(string branchRoot, DiscordBranch branch)
        {
            string name = GetProcessName(branch);
            return Path.Combine(GetLatestAppDirectory(branchRoot), name + ".exe");
        }

        public static string GetProcessName(DiscordBranch branch)
        {
            switch (branch)
            {
                case DiscordBranch.Stable:
                    return "Discord";

                case DiscordBranch.Canary:
                    return "DiscordCanary";

                case DiscordBranch.PTB:
                    return "DiscordPTB";

                default:
                    throw new ArgumentOutOfRangeException(nameof(branch), branch, "Unknown Discord branch.");
            }
        }

        public static bool IsRunning(DiscordBranch branch)
        {
            string processName = GetProcessName(branch);
            var processes = Process.GetProcessesByName(processName);

            try
            {
                return processes.Length > 0;
            }
            finally
            {
                foreach (var process in processes)
                    process.Dispose();
            }
        }

        public static void Kill(DiscordBranch branch)
        {
            string processName = GetProcessName(branch);
            var processes = Process.GetProcessesByName(processName);

            try
            {
                foreach (var process in processes)
                {
                    try
                    {
                        if (!process.HasExited)
                            process.Kill();
                    }
                    catch (InvalidOperationException) when (process.HasExited)
                    {
                        // the process exited between the check and kill
                    }
                    catch (Win32Exception ex) when (ex.NativeErrorCode == 5 && process.HasExited)
                    {
                        // windows also returns access denied for a terminated process
                    }
                }
            }
            finally
            {
                foreach (var process in processes)
                    process.Dispose();
            }
        }

        /// <summary>resolves the latest app directory for an installed discord branch.</summary>
        /// <exception cref="DirectoryNotFoundException">the root or app directory is missing.</exception>
        public static DiscordInstallation GetInstallation(DiscordBranch branch)
            => DiscordInstallation.Open(GetRootDirectory(branch), branch);

        /// <summary>kills the branch's processes and waits up to timeoutMs for them to exit.</summary>
        public static bool KillAndWait(DiscordBranch branch, int timeoutMs = 5000, int retryIntervalMs = 150)
        {
            if (timeoutMs < 0)
                throw new ArgumentOutOfRangeException(nameof(timeoutMs));

            if (retryIntervalMs <= 0)
                throw new ArgumentOutOfRangeException(nameof(retryIntervalMs));

            var clock = Stopwatch.StartNew();

            do
            {
                Kill(branch);

                if (!IsRunning(branch))
                    return true;

                int remainingMs = timeoutMs - (int)clock.ElapsedMilliseconds;
                if (remainingMs <= 0)
                    break;

                Thread.Sleep(Math.Min(retryIntervalMs, remainingMs));
            }
            while (clock.ElapsedMilliseconds < timeoutMs);

            return !IsRunning(branch);
        }

        /// <summary>matches both the branch directory and its exact executable name.</summary>
        public static bool IsTargetExecutable(string rootDirectory, string exeName)
        {
            if (string.IsNullOrEmpty(rootDirectory) || string.IsNullOrWhiteSpace(exeName))
                return false;

            string directoryName = Path.GetFileName(rootDirectory.TrimEnd(
                Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));
            string executableName = Path.GetFileName(exeName.Trim().Trim('"'));

            foreach (DiscordBranch branch in Enum.GetValues(typeof(DiscordBranch)))
            {
                string name = GetProcessName(branch);
                if (name.Equals(directoryName, StringComparison.OrdinalIgnoreCase))
                    return (name + ".exe").Equals(executableName, StringComparison.OrdinalIgnoreCase);
            }

            return false;
        }

        public static void Launch(DiscordBranch branch)
        {
            string branchRoot = GetRootDirectory(branch);
            string executablePath = GetExecutablePath(branchRoot, branch);

            if (!File.Exists(executablePath))
                throw new FileNotFoundException("Discord executable not found!", executablePath);

            Process.Start(executablePath);
        }
    }
}
