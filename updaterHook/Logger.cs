extern alias framework;

using Microsoft.Win32;
using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Reflection;
using CallerFilePathAttribute = framework::System.Runtime.CompilerServices.CallerFilePathAttribute;
using CallerLineNumberAttribute = framework::System.Runtime.CompilerServices.CallerLineNumberAttribute;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Threading;

namespace Droute.UpdaterHook
{
    public static class Logger
    {
        private static readonly object Sync = new object();
        private static readonly Stopwatch Clock = Stopwatch.StartNew();
        private static string _directory;
        private static string _session;
        private static int _pid;
        private static int _level = 2;
        private static Mutex _writerMutex;

        [DllImport("kernel32.dll")]
        private static extern uint GetCurrentThreadId();

        static Logger()
        {
            try
            {
                using (var process = Process.GetCurrentProcess())
                {
                    _pid = process.Id;
                    _session = process.StartTime.ToUniversalTime().ToFileTimeUtc().ToString("X", CultureInfo.InvariantCulture);
                }
                string root = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);
                string name = Path.GetFileName(root.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));
                string branch = name.Equals("Discord", StringComparison.OrdinalIgnoreCase) ? "stable"
                    : name.Equals("DiscordCanary", StringComparison.OrdinalIgnoreCase) ? "canary"
                    : name.Equals("DiscordPTB", StringComparison.OrdinalIgnoreCase) ? "ptb" : "unknown";
                string localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
                if (string.IsNullOrEmpty(localAppData))
                    throw new DirectoryNotFoundException("LocalAppData is unavailable.");
                _directory = Path.Combine(localAppData, "droute", "logs", branch);
                using (var sha = SHA256.Create())
                {
                    string key = BitConverter.ToString(sha.ComputeHash(Encoding.UTF8.GetBytes(_directory.ToUpperInvariant()))).Replace("-", "");
                    _writerMutex = new Mutex(false, @"Global\droute.updater.log." + key);
                }
                try
                {
                    using (var registry = Registry.CurrentUser.OpenSubKey(@"Software\droute"))
                    {
                        object value = registry?.GetValue("LogLevel");
                        if (value is int && (int)value >= 0 && (int)value <= 4) _level = (int)value;
                    }
                }
                catch (Exception ex)
                {
                    System.Diagnostics.Trace.WriteLine("droute updater log settings unavailable, using INFO: " + ex);
                }
                Log(2, "INFO", $"event=updater.session.start branch={branch} root=\"{root}\" " +
                    $"version={typeof(Logger).Assembly.GetName().Version} runtime={Environment.Version} " +
                    $"architecture={(Environment.Is64BitProcess ? "x64" : "x86")} log_level={_level} " +
                    $"utc_offset_minutes={TimeZoneInfo.Local.GetUtcOffset(DateTime.Now).TotalMinutes}", "Logger.cs", 0);
            }
            catch (Exception ex)
            {
                // Logging failures must remain visible without interrupting the updater.
                System.Diagnostics.Trace.WriteLine("droute updater logging initialization failed: " + ex);
            }
        }

        public static void Trace(string message, [CallerFilePath] string file = "", [CallerLineNumber] int line = 0)
            => Log(0, "TRACE", message, file, line);
        public static void Debug(string message, [CallerFilePath] string file = "", [CallerLineNumber] int line = 0)
            => Log(1, "DEBUG", message, file, line);
        public static void Info(string message, [CallerFilePath] string file = "", [CallerLineNumber] int line = 0)
            => Log(2, "INFO", message, file, line);
        public static void Warning(string message, [CallerFilePath] string file = "", [CallerLineNumber] int line = 0)
            => Log(3, "WARN", message, file, line);
        public static void Error(string message, [CallerFilePath] string file = "", [CallerLineNumber] int line = 0)
            => Log(4, "ERROR", message, file, line);

        private static void Log(int severity, string level, string message, string file, int line)
        {
            if (severity < _level) return;
            string record = null;
            bool acquired = false;
            try
            {
                lock (Sync)
                {
                    DateTime now = DateTime.Now;
                    // Keep exceptions, including inner exceptions and stacks, in one log record.
                    string text = (message ?? "").Replace("\r", "\\r").Replace("\n", "\\n");
                    record = $"[{now.ToString("yyyy-MM-dd HH:mm:ss.fff", CultureInfo.InvariantCulture)}] " +
                        $"[PID:{_pid}] [TID:{GetCurrentThreadId()}] [SESSION:{_session}] [UP:{Clock.ElapsedMilliseconds}] " +
                        $"[{level}] [{Path.GetFileName(file)}:{line}] {text}{Environment.NewLine}";
                    if (_writerMutex == null || string.IsNullOrEmpty(_directory))
                        throw new InvalidOperationException("Log writer was not initialized.");
                    try { acquired = _writerMutex.WaitOne(1000); }
                    catch (AbandonedMutexException) { acquired = true; }
                    if (!acquired) throw new TimeoutException("Timed out acquiring the shared log writer.");
                    Directory.CreateDirectory(_directory);
                    string path = Path.Combine(_directory, "updater-" + now.ToString("yyyy-MM-dd", CultureInfo.InvariantCulture) + ".log");
                    byte[] bytes = Encoding.UTF8.GetBytes(record);
                    using (var stream = new FileStream(path, FileMode.OpenOrCreate, FileAccess.Write,
                        FileShare.ReadWrite | FileShare.Delete))
                    {
                        stream.Seek(0, SeekOrigin.End);
                        stream.Write(bytes, 0, bytes.Length);
                    }
                }
            }
            catch (Exception ex)
            {
                System.Diagnostics.Trace.WriteLine("droute updater log write failed: " + ex);
                if (record != null) System.Diagnostics.Trace.WriteLine(record);
            }
            finally
            {
                if (acquired)
                {
                    try { _writerMutex.ReleaseMutex(); }
                    catch (Exception ex) { System.Diagnostics.Trace.WriteLine("droute updater log unlock failed: " + ex); }
                }
            }
        }
    }
}
