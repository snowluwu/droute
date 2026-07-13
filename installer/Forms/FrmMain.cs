using Droute.Core;
using Droute.Installer.Classes;
using Droute.Installer.Properties;
using System;
using System.Diagnostics;
using System.IO;
using System.Windows.Forms;

namespace Droute.Installer.Forms
{
    public partial class FrmMain : Form
    {
        private Config _cfg = null;
        private DiscordManager.Branches _selectedBranch = DiscordManager.Branches.Stable;

        public FrmMain()
        {
            InitializeComponent();

            _cfg = new Config();
        }

        private void authCheckBox_CheckedChanged(object sender, EventArgs e)
        {
            authPanel.Enabled = authCheckBox.Checked;
        }

        private void FrmMain_Load(object sender, EventArgs e)
        {
            #region [ Set values from Registry Config ]

            hostTextBox.Text = _cfg.Host;
            portNumeric.Value = _cfg.Port;
            userTextBox.Text = _cfg.User;
            passwordTextBox.Text = _cfg.Password;

            #endregion

            if (string.IsNullOrEmpty(_cfg.User) && string.IsNullOrEmpty(_cfg.Password))
                authCheckBox.Checked = false;

            this.LoadDiscordActionSettings();

            #region [ Set Version (aboutTab) ]

            var versionInfo = new Version(Application.ProductVersion);
            versionLabel.Text = $"v. {versionInfo.Major}.{versionInfo.Minor}.{versionInfo.Build}";

            #endregion

            #region [ Load Branches ]

            branchesComboBox.Items.Clear();
            branchesComboBox.DropDownStyle = ComboBoxStyle.DropDownList;

            var availableBranches = DiscordManager.GetInstalledBranches();
            if (availableBranches.Count == 0)
            {
                branchesComboBox.Items.Add(DiscordManager.Branches.Stable);
                branchesComboBox.SelectedIndex = 0;
                _selectedBranch = DiscordManager.Branches.Stable;
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
            bool restartDiscord = Settings.Default.AutoRestartConfig && DiscordManager.IsDiscordRunning(_selectedBranch);

            this.ApplyConfig();

            if (restartDiscord)
                this.RestartDiscord(_selectedBranch);

            MessageBox.Show("Config has been applied!", "Droute: apply changes", MessageBoxButtons.OK, MessageBoxIcon.Information);
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
            string localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            string path = Path.Combine(localAppData, "Temp", "droute.log");

            try { Process.Start(path); }
            catch (Exception ex) { Trace.WriteLine($"error during open log file: {ex.ToString()}"); }
        }

        private void installPatchButton_Click(object sender, EventArgs e)
        {
            this.ApplyConfig();
            this.HandleAction(FrmPatch.PatchAction.Install);
        }

        private void removePatchButton_Click(object sender, EventArgs e) 
            => this.HandleAction(FrmPatch.PatchAction.Remove);

        private void ApplyConfig()
        {
            _cfg.Host = hostTextBox.Text.Trim();
            _cfg.Port = (int)portNumeric.Value;
            _cfg.User = authCheckBox.Checked ? userTextBox.Text : string.Empty;
            _cfg.Password = authCheckBox.Checked ? passwordTextBox.Text : string.Empty;
            _cfg.Apply();
        }

        private void HandleAction(FrmPatch.PatchAction action)
        {
            _selectedBranch = this.GetSelectedBranch();

            bool restartDiscord = action == FrmPatch.PatchAction.Install &&
                Settings.Default.AutoRestartPatch &&
                DiscordManager.IsDiscordRunning(_selectedBranch);

            if (Settings.Default.AutoRestartPatch && !this.CloseDiscord(_selectedBranch))
                return;

            using (var frm = new FrmPatch(action, _selectedBranch))
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

        private DiscordManager.Branches GetSelectedBranch()
        {
            if (branchesComboBox.SelectedItem is DiscordManager.Branches branch)
                return branch;

            return DiscordManager.Branches.Stable;
        }

        private void RestartDiscord(DiscordManager.Branches branch)
        {
            if (this.CloseDiscord(branch))
                this.LaunchDiscord(branch);
        }

        private bool CloseDiscord(DiscordManager.Branches branch)
        {
            try
            {
                if (DiscordTools.CloseWait(branch))
                    return true;

                MessageBox.Show("Discord did not exit in time. Close it manually and try again.",
                    "Unable to close Discord", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }
            catch (Exception ex)
            {
                Trace.WriteLine($"error during Discord shutdown: {ex}");
                MessageBox.Show(ex.Message, "Unable to close Discord", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }

            return false;
        }

        private void LaunchDiscord(DiscordManager.Branches branch)
        {
            try
            {
                try
                {
                    string branchRoot = DiscordManager.GetBranchRoot(branch);
                    string exePath = DiscordManager.GetDiscordExecutablePath(branchRoot, branch);

                    ProcessStartInfo si = new ProcessStartInfo
                    {
                        FileName = "cmd.exe",
                        Arguments = $"/c runas /trustlevel:0x20000 \"{exePath}\"",
                        CreateNoWindow = true,
                        UseShellExecute = false
                    };

                    Process.Start(si);
                }
                catch
                {
                    Trace.WriteLine($"failed to run discord as user, try default method");
                    DiscordManager.Launch(branch);
                }
            }
            catch (Exception ex)
            {
                Trace.WriteLine($"error during Discord launch: {ex.ToString()}");
                MessageBox.Show(ex.Message, "Unable to launch Discord", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }
        }

        private void resetCfgBtn_Click(object sender, EventArgs e)
        {
            DialogResult result = MessageBox.Show
            (
                "Droute settings will be deleted from registry. At the same time, patch itself will not be affected.", 
                "Are you sure?", 
                MessageBoxButtons.YesNo, 
                MessageBoxIcon.Warning
            );

            if (result == DialogResult.Yes)
            {
                _cfg.Reset();
                Application.Restart();
            }
        }
    }
}
