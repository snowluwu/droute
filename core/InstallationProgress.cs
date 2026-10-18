using System;

namespace Droute.Core
{
    public enum InstallationStage
    {
        CopyingProxy,
        PatchingProxy,
        StagingLibrary,
        PublishingLibrary,
        PublishingProxy,
        PatchInstalled,
        StagingUpdaterHook,
        PublishingUpdaterHook,
        PublishingUpdaterConfig,
        UpdaterHookInstalled,
        PatchUninstalled,
        UpdaterHookUninstalled,
        TemporaryFileDeleted,
        CleanupFailed,
        StagingUpdaterConfig,
        RemovingFile
    }

    /// <summary>operation details delivered synchronously on the calling thread.</summary>
    public sealed class InstallationProgress
    {
        public InstallationStage Stage { get; }
        public string Path { get; }

        /// <summary>a cleanup warning; operation failures are thrown by the installer.</summary>
        public Exception Error { get; }

        internal InstallationProgress(InstallationStage stage, string path, Exception error = null)
        {
            Stage = stage;
            Path = path;
            Error = error;
        }
    }
}
