using Droute.Core;
using System;
using System.Diagnostics;
using System.Threading;

namespace Droute.Installer.Classes
{
    internal static class DiscordTools
    {
        public static bool CloseWait(DiscordManager.Branches branch, int timeoutMs = 5000, int retryIntervalMs = 150)
        {
            if (timeoutMs < 0)
                throw new ArgumentOutOfRangeException(nameof(timeoutMs));

            if (retryIntervalMs <= 0)
                throw new ArgumentOutOfRangeException(nameof(retryIntervalMs));

            var stopwatch = Stopwatch.StartNew();

            do
            {
                DiscordManager.Close(branch);

                if (!DiscordManager.IsDiscordRunning(branch))
                    return true;

                int remainingMs = timeoutMs - (int)stopwatch.ElapsedMilliseconds;
                if (remainingMs <= 0)
                    break;

                Thread.Sleep(Math.Min(retryIntervalMs, remainingMs));
            }
            while (stopwatch.ElapsedMilliseconds < timeoutMs);

            return !DiscordManager.IsDiscordRunning(branch);
        }
    }
}
