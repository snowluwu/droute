using Microsoft.Win32;
using System;
using System.Reflection;

namespace Droute.Installer.Classes
{
    internal class ProxySettings
    {
        public enum LogLevelValue
        {
            Trace = 0,
            Debug = 1,
            Info = 2,
            Warn = 3,
            Error = 4
        }

        private const string RegistryPath = @"Software\droute";

        #region [ settings ]

        public string Host { get; set; } = "127.0.0.1";
        public int Port { get; set; } = 1080;

        public string User { get; set; } = string.Empty;
        public string Password { get; set; } = string.Empty;

        public int ConnectTimeout { get; set; } = 5000;
        public int ReconnectInterval { get; set; } = 3000;
        public LogLevelValue LogLevel { get; set; } = LogLevelValue.Info;

        #endregion

        public ProxySettings(bool load = true)
        {
            if (load)
                Load();
        }

        public void Load()
        {
            using (RegistryKey rk = Registry.CurrentUser.OpenSubKey(RegistryPath))
            {
                if (rk == null)
                    return;

                foreach (var prop in GetType().GetProperties(BindingFlags.Public | BindingFlags.Instance))
                {
                    object value = rk.GetValue(prop.Name);
                    if (value == null)
                        continue;

                    try
                    {
                        if (prop.PropertyType.IsEnum)
                        {
                            object enumValue = Enum.ToObject(prop.PropertyType, value);
                            if (!Enum.IsDefined(prop.PropertyType, enumValue))
                                throw new ArgumentException("Unknown value.");

                            prop.SetValue(this, enumValue);
                        }
                        else if (prop.PropertyType == typeof(int))
                            prop.SetValue(this, Convert.ToInt32(value));
                        else
                            prop.SetValue(this, value.ToString());
                    }
                    catch (Exception ex)
                    {
                        throw new InvalidOperationException($"Unable to read setting {prop.Name}.", ex);
                    }
                }
            }
        }

        public void Save()
        {
            Validate();

            using (RegistryKey rk = Registry.CurrentUser.CreateSubKey(RegistryPath))
            {
                foreach (var prop in GetType().GetProperties(BindingFlags.Public | BindingFlags.Instance))
                {
                    object value = prop.GetValue(this);
                    if (prop.PropertyType == typeof(int) || prop.PropertyType.IsEnum)
                        rk.SetValue(prop.Name, (int)value, RegistryValueKind.DWord);
                    else
                        rk.SetValue(prop.Name, value?.ToString() ?? string.Empty, RegistryValueKind.String);
                }
            }
        }

        public void Reset()
        {
            Registry.CurrentUser.DeleteSubKeyTree(RegistryPath, throwOnMissingSubKey: false);

            var defaults = new ProxySettings(false);
            foreach (var prop in GetType().GetProperties(BindingFlags.Public | BindingFlags.Instance))
                prop.SetValue(this, prop.GetValue(defaults));
        }

        public void Validate()
        {
            if (string.IsNullOrWhiteSpace(Host))
                throw new ArgumentException("Proxy host must not be empty.");

            Host = Host.Trim();

            if (Port < 1 || Port > 65535)
                throw new ArgumentOutOfRangeException(nameof(Port), "Proxy port must be between 1 and 65535.");

            if (ConnectTimeout < 100)
                throw new ArgumentOutOfRangeException(nameof(ConnectTimeout), "Connection timeout must be at least 100 ms.");

            if (ReconnectInterval < 100)
                throw new ArgumentOutOfRangeException(nameof(ReconnectInterval), "Reconnect interval must be at least 100 ms.");

            if (!Enum.IsDefined(typeof(LogLevelValue), LogLevel))
                throw new ArgumentException("Unknown log level.", nameof(LogLevel));
        }
    }
}
