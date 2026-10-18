using System;
using System.IO;

namespace Droute.Core
{
    /// <summary>paths resolved for one discord version; open again after discord updates.</summary>
    public sealed class DiscordInstallation
    {
        public DiscordBranch Branch { get; }
        public string RootDirectory { get; }
        public string AppDirectory { get; }
        public string ExecutablePath { get; }

        private DiscordInstallation(DiscordBranch branch, string rootDirectory, string appDirectory)
        {
            Branch = branch;
            RootDirectory = rootDirectory;
            AppDirectory = appDirectory;
            ExecutablePath = Path.Combine(appDirectory, Discord.GetProcessName(branch) + ".exe");
        }

        /// <summary>resolves a standard or custom root directory using an explicit branch.</summary>
        /// <exception cref="DirectoryNotFoundException">no app version exists in the root directory.</exception>
        public static DiscordInstallation Open(string rootDirectory, DiscordBranch branch)
        {
            Discord.GetProcessName(branch);

            if (string.IsNullOrWhiteSpace(rootDirectory))
                throw new ArgumentException("Root directory is required.", nameof(rootDirectory));

            string root = Path.GetFullPath(rootDirectory);
            return new DiscordInstallation(branch, root, Discord.GetLatestAppDirectory(root));
        }
    }
}
