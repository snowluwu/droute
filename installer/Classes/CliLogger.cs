using System;
using System.IO;

namespace Droute.Installer.Classes
{
    internal static class CliLogger
    {
        private static readonly object ConsoleSync = new object();
        public static void WriteOk(string message)
            => Write("OK", ConsoleColor.Green, message, Console.Out);

        public static void WriteError(string message)
            => Write("ERROR", ConsoleColor.Red, message, Console.Error);

        public static void Write(MessageLevel level, string message)
        {
            switch (level)
            {
                case MessageLevel.Error:
                    WriteError(message);
                    break;
                case MessageLevel.Warning:
                    Write("WARN", ConsoleColor.Yellow, message, Console.Out);
                    break;
                case MessageLevel.Success:
                    WriteOk(message);
                    break;
                case MessageLevel.Stage:
                    Write("STAGE", ConsoleColor.Cyan, message, Console.Out);
                    break;
                case MessageLevel.Step:
                    Write(">", ConsoleColor.DarkGray, message, Console.Out);
                    break;
                case MessageLevel.Removal:
                    Write("<", ConsoleColor.DarkGray, message, Console.Out);
                    break;
                default:
                    Write("INFO", ConsoleColor.Blue, message, Console.Out);
                    break;
            }
        }

        private static void Write(string level, ConsoleColor color, string message, TextWriter writer)
        {
            lock (ConsoleSync)
            {
                bool redirected = ReferenceEquals(writer, Console.Error)
                    ? Console.IsErrorRedirected
                    : Console.IsOutputRedirected;

                if (redirected)
                {
                    writer.WriteLine($"[ {level} ] {message}");
                    return;
                }

                ConsoleColor previousForeground = Console.ForegroundColor;

                try
                {
                    Console.ForegroundColor = ConsoleColor.DarkGray;
                    writer.Write($"[{DateTime.Now:HH:mm:ss}] ");

                    Console.ForegroundColor = color;
                    writer.Write($"[ {level} ] ");
                    writer.WriteLine(message);
                }
                finally
                {
                    Console.ForegroundColor = previousForeground;
                }
            }
        }
    }
}
