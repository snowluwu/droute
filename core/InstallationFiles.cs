using System;
using System.IO;

namespace Droute.Core
{
    /// <summary>files required for a full installation, supplied by the caller.</summary>
    public sealed class InstallationFiles
    {
        public byte[] NativeLibrary { get; }
        public byte[] UpdaterHook { get; }
        public string UpdaterConfig { get; }

        private InstallationFiles(byte[] nativeLibrary, byte[] updaterHook, string updaterConfig)
        {
            if (nativeLibrary == null || nativeLibrary.Length == 0)
                throw new ArgumentException("Native library is required.", nameof(nativeLibrary));

            if (updaterHook == null || updaterHook.Length == 0)
                throw new ArgumentException("Updater hook is required.", nameof(updaterHook));

            if (string.IsNullOrWhiteSpace(updaterConfig))
                throw new ArgumentException("Updater config is required.", nameof(updaterConfig));

            NativeLibrary = nativeLibrary;
            UpdaterHook = updaterHook;
            UpdaterConfig = updaterConfig;
        }

        /// <summary>uses the supplied buffers without copying; keep them unchanged during installation.</summary>
        public static InstallationFiles FromBytes(byte[] nativeLibrary, byte[] updaterHook, string updaterConfig)
            => new InstallationFiles(nativeLibrary, updaterHook, updaterConfig);

        /// <summary>reads all installation files before making any changes to discord.</summary>
        /// <exception cref="IOException">an installation file could not be read.</exception>
        public static InstallationFiles FromFiles(string nativeLibraryPath, string updaterHookPath, string updaterConfigPath)
            => FromBytes(File.ReadAllBytes(nativeLibraryPath), File.ReadAllBytes(updaterHookPath),
                File.ReadAllText(updaterConfigPath));
    }
}
