using System;
using System.IO;

namespace Droute.Core
{
    /// <summary>files required for a full installation, supplied by the caller.</summary>
    public sealed class InstallationFiles
    {
        public byte[] NativeLibrary { get; }
        public byte[] WeaveLibrary { get; }
        public byte[] UpdaterHook { get; }
        public string UpdaterConfig { get; }

        private InstallationFiles(byte[] nativeLibrary, byte[] weaveLibrary, byte[] updaterHook, string updaterConfig)
        {
            if (nativeLibrary == null || nativeLibrary.Length == 0)
                throw new ArgumentException("Native library is required.", nameof(nativeLibrary));

            if (updaterHook == null || updaterHook.Length == 0)
                throw new ArgumentException("Updater hook is required.", nameof(updaterHook));

            if (string.IsNullOrWhiteSpace(updaterConfig))
                throw new ArgumentException("Updater config is required.", nameof(updaterConfig));

            if (weaveLibrary == null || weaveLibrary.Length == 0)
                throw new ArgumentException("Weave library is required.", nameof(weaveLibrary));

            NativeLibrary = nativeLibrary;
            WeaveLibrary = weaveLibrary;
            UpdaterHook = updaterHook;
            UpdaterConfig = updaterConfig;
        }

        /// <summary>uses the supplied buffers without copying; keep them unchanged during installation.</summary>
        public static InstallationFiles FromBytes(byte[] nativeLibrary, byte[] weaveLibrary,
            byte[] updaterHook, string updaterConfig)
            => new InstallationFiles(nativeLibrary, weaveLibrary, updaterHook, updaterConfig);

        /// <summary>reads all installation files before making any changes to discord.</summary>
        /// <exception cref="IOException">an installation file could not be read.</exception>
        public static InstallationFiles FromFiles(string nativeLibraryPath, string weaveLibraryPath,
            string updaterHookPath, string updaterConfigPath)
            => FromBytes(File.ReadAllBytes(nativeLibraryPath), File.ReadAllBytes(weaveLibraryPath),
                File.ReadAllBytes(updaterHookPath),
                File.ReadAllText(updaterConfigPath));
    }
}
