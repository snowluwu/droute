using Droute.Core;
using Droute.Installer.Classes;
using System;
using System.Drawing;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace Droute.Installer.Forms
{
    public partial class PatchForm : Form
    {
        public enum PatchAction
        {
            Install,
            Uninstall
        }

        public bool IsSuccessful { get; private set; }
        public DiscordBranch SelectedBranch { get; set; } = DiscordBranch.Stable;

        private bool _isWorking = false;
        private readonly PatchAction _action;

        public PatchForm(PatchAction action, DiscordBranch branch)
        {
            InitializeComponent();

            _action = action;

            this.SelectedBranch = branch;
            this.Text = _action == PatchAction.Install ? "Droute: Installing Patch..." : "Droute: Removing Patch...";
        }

        private async void PatchForm_Shown(object sender, EventArgs e)
            => await ExecuteOperation();

        private async Task ExecuteOperation()
        {
            _isWorking = true;

            ClearJournal();
            UpdateProgress(0);

            try
            {
                IsSuccessful = await Task.Run(() =>
                {
                    return _action == PatchAction.Install
                        ? PatchActions.Install(SelectedBranch, WriteJournal, UpdateProgress)
                        : PatchActions.Uninstall(SelectedBranch, WriteJournal, UpdateProgress);
                });
            }
            catch (Exception ex)
            {
                WriteJournal(MessageLevel.Error, ex.Message);
                IsSuccessful = false;
            }
            finally
            {
                _isWorking = false;
            }

            if (IsSuccessful)
            {
                this.Text = "Droute: Operation completed!";
            }
            else
            {
                this.Text = "Droute: Operation failed!";
            }
        }

        private void PatchForm_FormClosing(object sender, FormClosingEventArgs e)
        {
            if (_isWorking)
                e.Cancel = true;
        }

        private void WriteJournal(MessageLevel level, string content)
        {
            if (journalRichBox.InvokeRequired)
            {
                journalRichBox.Invoke(new Action(() => WriteJournal(level, content)));
                return;
            }

            Color color;
            string tag;
            switch (level)
            {
                case MessageLevel.Error:
                    color = Color.DarkRed;
                    tag = "ERROR";
                    break;
                case MessageLevel.Warning:
                    color = Color.DarkGoldenrod;
                    tag = "WARN";
                    break;
                case MessageLevel.Success:
                    color = Color.DarkGreen;
                    tag = "OK";
                    break;
                case MessageLevel.Stage:
                    color = Color.DarkCyan;
                    tag = "STAGE";
                    break;
                case MessageLevel.Step:
                    color = Color.DimGray;
                    tag = ">";
                    break;
                case MessageLevel.Removal:
                    color = Color.DimGray;
                    tag = "<";
                    break;
                default:
                    color = Color.Blue;
                    tag = "INFO";
                    break;
            }

            journalRichBox.SelectionStart = journalRichBox.TextLength;
            journalRichBox.SelectionLength = 0;
            journalRichBox.SelectionFont = journalRichBox.Font;
            journalRichBox.SelectionColor = Color.DimGray;
            journalRichBox.AppendText($"[{DateTime.Now:HH:mm:ss}] ");

            journalRichBox.SelectionColor = color;

            if (level == MessageLevel.Step || level == MessageLevel.Removal)
            {
                journalRichBox.AppendText($"[ {tag} ]");
            }
            else
            {
                using (var font = new Font(journalRichBox.Font, FontStyle.Bold))
                {
                    journalRichBox.SelectionFont = font;
                    journalRichBox.AppendText($"[ {tag} ]");
                }
            }

            journalRichBox.SelectionFont = journalRichBox.Font;
            journalRichBox.AppendText($" {content}{Environment.NewLine}");
            journalRichBox.SelectionColor = journalRichBox.ForeColor;
            journalRichBox.ScrollToCaret();
        }

        public void UpdateProgress(int value)
        {
            if (progressBar.InvokeRequired)
                progressBar.Invoke(new Action(() => progressBar.Value = value));
            else
                progressBar.Value = value;
        }

        private void ClearJournal()
        {
            if (journalRichBox.InvokeRequired)
                journalRichBox.Invoke(new Action(() => journalRichBox.Clear()));
            else
                journalRichBox.Clear();
        }
    }
}
