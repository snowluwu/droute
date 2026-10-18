using Droute.Core;
using Droute.Installer.Classes;
using Droute.Installer.Properties;
using System;
using System.Diagnostics;
using System.IO;
using System.Windows.Forms;

namespace Droute.Installer.Forms
{
    public partial class MainForm : Form
    {
        private ProxySettings _cfg = null;
        private DiscordBranch _selectedBranch = DiscordBranch.Stable;

        public MainForm()
        {
            InitializeComponent();

            _cfg = new ProxySettings(false);
            try
            {
                _cfg.Load();
                _cfg.Validate();
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Error on load configuration: \"{ex.Message}\"\nDefault settings will be loaded.", "Error on load configuration",
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
                _cfg = new ProxySettings(false);
            }
        }

        private void authCheckBox_CheckedChanged(object sender, EventArgs e)
        {
            authPanel.Enabled = authCheckBox.Checked;
        }

        private void MainForm_Load(object sender, EventArgs e)
        {
            #region [ proxy settings ]

            hostTextBox.Text = _cfg.Host;
            portNumeric.Value = _cfg.Port;
            userTextBox.Text = _cfg.User;
            passwordTextBox.Text = _cfg.Password;

            #endregion

            if (string.IsNullOrEmpty(_cfg.User) && string.IsNullOrEmpty(_cfg.Password))
                authCheckBox.Checked = false;

            this.LoadDiscordActionSettings();

            #region [ version ]

            var versionInfo = new Version(Application.ProductVersion);
            versionLabel.Text = $"v. {versionInfo.Major}.{versionInfo.Minor}.{versionInfo.Build}";

            #endregion

            #region [ discord branches ]

            branchesComboBox.Items.Clear();
            branchesComboBox.DropDownStyle = ComboBoxStyle.DropDownList;

            var availableBranches = Discord.GetInstalledBranches();
            if (availableBranches.Count == 0)
            {
                branchesComboBox.Items.Add(DiscordBranch.Stable);
                branchesComboBox.SelectedIndex = 0;
                _selectedBranch = DiscordBranch.Stable;
                return;
            }

            foreach (var branch in availableBranches)
                branchesComboBox.Items.Add(branch);

            branchesComboBox.SelectedItem = availableBranches[0];

            _selectedBranch = availableBranches[0];

            #endregion
        }

        private void applyCfgButton_Click(object sender, EventArgs e)
        {
            _selectedBranch = GetSelectedBranch();
            bool restartDiscord = Settings.Default.AutoRestartConfig && Discord.IsRunning(_selectedBranch);

            if (!this.SaveSettings())
                return;

            if (restartDiscord)
                this.RestartDiscord(_selectedBranch);

            MessageBox.Show("Config has been applied!",
                "Droute: apply changes", MessageBoxButtons.OK, MessageBoxIcon.Information);
        }

        private void discordActionsCheckbox_CheckedChanged(object sender, EventArgs e)
        {
            Settings.Default.AutoRestartPatch = autoRestartPatchCheckbox.Checked;
            Settings.Default.AutoRestartConfig = autoRestartConfigCheckbox.Checked;
            Settings.Default.Save();
        }

        private void repoLink_LinkClicked(object sender, LinkLabelLinkClickedEventArgs e)
        {
            Process.Start("https://github.com/snowluwu/droute");
        }

        private void licenseLink_LinkClicked(object sender, LinkLabelLinkClickedEventArgs e)
        {
            Process.Start("https://github.com/snowluwu/droute/blob/master/LICENSE.txt");
        }

        private void inspiredLink_LinkClicked(object sender, LinkLabelLinkClickedEventArgs e)
        {
            Process.Start("https://github.com/runetfreedom/force-proxy");
        }

        private void openLogsButton_Click(object sender, EventArgs e)
        {
            try
            {
                string localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
                if (string.IsNullOrEmpty(localAppData))
                    throw new DirectoryNotFoundException("LocalAppData is unavailable.");

                string branch = GetSelectedBranch().ToString().ToLowerInvariant();
                string path = Path.Combine(localAppData, "droute", "logs", branch);
                Directory.CreateDirectory(path);
                Process.Start(new ProcessStartInfo(path) { UseShellExecute = true });
            }
            catch (Exception ex)
            {
                Trace.WriteLine($"error opening logs folder: {ex}");
                MessageBox.Show($"Unable to open the logs folder.\n\n{ex.Message}",
                    "Droute: logs", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        private void installPatchButton_Click(object sender, EventArgs e)
        {
            if (!this.SaveSettings())
                return;

            this.HandleAction(PatchForm.PatchAction.Install);
        }

        private void removePatchButton_Click(object sender, EventArgs e)
            => this.HandleAction(PatchForm.PatchAction.Uninstall);

        private bool SaveSettings()
        {
            try
            {
                _cfg.Host = hostTextBox.Text.Trim();
                _cfg.Port = (int)portNumeric.Value;
                _cfg.User = authCheckBox.Checked ? userTextBox.Text : string.Empty;
                _cfg.Password = authCheckBox.Checked ? passwordTextBox.Text : string.Empty;
                _cfg.Save();
                return true;
            }
            catch (Exception ex)
            {
                MessageBox.Show(ex.Message, "Droute: configuration", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return false;
            }
        }

        private void HandleAction(PatchForm.PatchAction action)
        {
            _selectedBranch = this.GetSelectedBranch();

            bool restartDiscord = action == PatchForm.PatchAction.Install &&
                Settings.Default.AutoRestartPatch &&
                Discord.IsRunning(_selectedBranch);

            if (Settings.Default.AutoRestartPatch && !this.CloseDiscord(_selectedBranch))
                return;

            using (var frm = new PatchForm(action, _selectedBranch))
            {
                frm.ShowDialog();

                if (restartDiscord)
                    this.LaunchDiscord(_selectedBranch);
            }
        }

        private void LoadDiscordActionSettings()
        {
            autoRestartPatchCheckbox.CheckedChanged -= discordActionsCheckbox_CheckedChanged;
            autoRestartConfigCheckbox.CheckedChanged -= discordActionsCheckbox_CheckedChanged;

            autoRestartPatchCheckbox.Checked = Settings.Default.AutoRestartPatch;
            autoRestartConfigCheckbox.Checked = Settings.Default.AutoRestartConfig;

            autoRestartPatchCheckbox.CheckedChanged += discordActionsCheckbox_CheckedChanged;
            autoRestartConfigCheckbox.CheckedChanged += discordActionsCheckbox_CheckedChanged;
        }

        private DiscordBranch GetSelectedBranch()
        {
            if (branchesComboBox.SelectedItem is DiscordBranch branch)
                return branch;

            return DiscordBranch.Stable;
        }

        private void RestartDiscord(DiscordBranch branch)
        {
            if (this.CloseDiscord(branch))
                this.LaunchDiscord(branch);
        }

        private bool CloseDiscord(DiscordBranch branch)
        {
            try
            {
                if (Discord.KillAndWait(branch))
                    return true;

                MessageBox.Show("Discord did not exit in time. Close it manually and try again.",
                    "Unable to close Discord", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }
            catch (Exception ex)
            {
                Trace.WriteLine($"error during Discord shutdown: {ex}");
                MessageBox.Show($"Unable to close Discord {branch}.\nError: \"{ex.Message}\"",
                    "Unable to close Discord", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }

            return false;
        }

        private void LaunchDiscord(DiscordBranch branch)
        {
            try
            {
                Discord.Launch(branch);
            }
            catch (Exception ex)
            {
                Trace.WriteLine($"error during Discord launch: {ex.ToString()}");
                MessageBox.Show(ex.Message, "Unable to launch Discord", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }
        }

        private void resetCfgBtn_Click(object sender, EventArgs e)
        {
            DialogResult result = MessageBox.Show("Droute settings will be deleted from registry. At the same time, patch itself will not be affected.",
                "Are you sure?", MessageBoxButtons.YesNo, MessageBoxIcon.Warning);

            if (result == DialogResult.Yes)
            {
                try
                {
                    _cfg.Reset();
                    Application.Restart();
                }
                catch (Exception ex)
                {
                    MessageBox.Show(ex.Message, "Droute: configuration", MessageBoxButtons.OK, MessageBoxIcon.Error);
                }
            }
        }
    }
}
