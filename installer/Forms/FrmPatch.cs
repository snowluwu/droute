using Droute.Core;
using Droute.Installer.Classes;
using System;
using System.Drawing;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace Droute.Installer.Forms
{
    public partial class FrmPatch : Form
    {
        public enum PatchAction { Install, Remove }
        public bool IsSuccessful { get; private set; }
        public DiscordManager.Branches SelectedBranch { get; set; } = DiscordManager.Branches.Stable;

        private bool _isWorking = false;
        private readonly PatchAction _action;

        public FrmPatch(PatchAction action, DiscordManager.Branches branch)
        {
            InitializeComponent();

            _action = action;

            this.SelectedBranch = branch;
            this.Text = _action == PatchAction.Install ? "Droute: Installing Patch..." : "Droute: Removing Patch...";
        }

        private async void FrmPatch_Shown(object sender, EventArgs e)
            => await ExecuteOperation();

        private async Task ExecuteOperation()
        {
            _isWorking = true;

            ClearJournal();
            UpdateProgress(0);

            PatchTools.OnLog += WriteJournal;
            PatchTools.OnProgressChanged += UpdateProgress;

            try
            {
                IsSuccessful = await Task.Run(() =>
                {
                    return _action == PatchAction.Install ? PatchTools.Install(SelectedBranch) : PatchTools.Remove(SelectedBranch);
                });
            }
            catch (Exception ex)
            {
                WriteJournal($"Error: {ex.Message}");
                IsSuccessful = false;
            }
            finally
            {
                PatchTools.OnLog -= WriteJournal;
                PatchTools.OnProgressChanged -= UpdateProgress;
                _isWorking = false;
            }

            if (IsSuccessful)
            {
                this.Text = "Droute: Operation completed!";
                WriteJournal("Done! You can now close this window.");
            }
            else
            {
                this.Text = "Droute: Operation failed!";
                WriteJournal("Error! Please check the log above before closing.");
            }
        }

        private void FrmPatch_FormClosing(object sender, FormClosingEventArgs e)
        {
            if (_isWorking)
                e.Cancel = true;
        }

        public void WriteJournal(string content)
        {
            if (journalRichBox.InvokeRequired)
            {
                journalRichBox.Invoke(new Action(() => WriteJournal(content)));
                return;
            }

            Color logColor = journalRichBox.ForeColor;

            bool makeTagBold = false;
            int tagLength = 0;

            if (content.StartsWith("[ STATUS ]"))
            {
                logColor = Color.Blue;
                makeTagBold = true;
                tagLength = "[ STATUS ]".Length;
            }
            else if (content.StartsWith("[ STAGE ]"))
            {
                logColor = Color.DarkCyan;
                makeTagBold = true;
                tagLength = "[ STAGE ]".Length;
            }
            else if (content.StartsWith("[ OK ]"))
            {
                logColor = Color.DarkGreen;
                makeTagBold = true;
                tagLength = "[ OK ]".Length;
            }
            else if (content.StartsWith("[ WARN ]"))
            {
                logColor = Color.DarkGoldenrod;
                makeTagBold = true;
                tagLength = "[ WARN ]".Length;
            }
            else if (content.StartsWith("[ FATAL ]"))
            {
                logColor = Color.DarkRed;
                makeTagBold = true;
                tagLength = "[ FATAL ]".Length;
            }
            else if (content.StartsWith("[ > ]") || content.StartsWith("[ < ]"))
            {
                logColor = Color.DimGray;
            }
            else if (content.StartsWith("Error", StringComparison.OrdinalIgnoreCase))
            {
                logColor = Color.DarkRed;
            }

            journalRichBox.SelectionStart = journalRichBox.TextLength;
            journalRichBox.SelectionLength = 0;

            journalRichBox.SelectionColor = Color.DimGray;
            journalRichBox.AppendText($"[{DateTime.Now:HH:mm:ss}] ");

            journalRichBox.SelectionColor = logColor;

            if (makeTagBold && tagLength > 0)
            {
                journalRichBox.SelectionFont = new Font(journalRichBox.Font, FontStyle.Bold);
                journalRichBox.AppendText(content.Substring(0, tagLength));

                journalRichBox.SelectionFont = new Font(journalRichBox.Font, FontStyle.Regular);
                journalRichBox.AppendText(content.Substring(tagLength));
            }
            else
            {
                journalRichBox.AppendText(content);
            }

            journalRichBox.AppendText(Environment.NewLine);
            journalRichBox.SelectionColor = journalRichBox.ForeColor;
            journalRichBox.SelectionFont = new Font(journalRichBox.Font, FontStyle.Regular);

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
