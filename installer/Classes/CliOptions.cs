using CommandLine;

namespace Droute.Installer.Classes
{
    internal class CliOptions
    {
        [Option('i', "install", Required = false, HelpText = "Install Droute.")]
        public bool Install { get; set; }

        [Option('u', "uninstall", Required = false, HelpText = "Remove Droute.")]
        public bool Uninstall { get; set; }

        [Option("branch", Required = false, Default = "stable", HelpText = "Discord branch: `stable`, `canary`, `ptb`.")]
        public string Branch { get; set; } = "stable";

        [Option("host", Required = false, HelpText = "Proxy address.")]
        public string Host { get; set; }

        [Option("port", Required = false, HelpText = "Proxy port.")]
        public int? Port { get; set; }

        [Option("user", Required = false, HelpText = "Proxy user.")]
        public string User { get; set; }

        [Option("password", Required = false, HelpText = "Proxy password.")]
        public string Password { get; set; }
    }
}
