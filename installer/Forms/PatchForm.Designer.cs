namespace Droute.Installer.Forms
{
    partial class PatchForm
    {
        /// <summary>
        /// Required designer variable.
        /// </summary>
        private System.ComponentModel.IContainer components = null;

        /// <summary>
        /// Clean up any resources being used.
        /// </summary>
        /// <param name="disposing">true if managed resources should be disposed; otherwise, false.</param>
        protected override void Dispose(bool disposing)
        {
            if (disposing && (components != null))
            {
                components.Dispose();
            }
            base.Dispose(disposing);
        }

        #region Windows Form Designer generated code

        /// <summary>
        /// Required method for Designer support - do not modify
        /// the contents of this method with the code editor.
        /// </summary>
        private void InitializeComponent()
        {
            System.ComponentModel.ComponentResourceManager resources = new System.ComponentModel.ComponentResourceManager(typeof(PatchForm));
            this.progressBar = new System.Windows.Forms.ProgressBar();
            this.journalRichBox = new System.Windows.Forms.RichTextBox();
            this.SuspendLayout();
            // 
            // progressBar
            // 
            this.progressBar.Anchor = ((System.Windows.Forms.AnchorStyles)(((System.Windows.Forms.AnchorStyles.Bottom | System.Windows.Forms.AnchorStyles.Left) 
            | System.Windows.Forms.AnchorStyles.Right)));
            this.progressBar.Location = new System.Drawing.Point(14, 238);
            this.progressBar.Margin = new System.Windows.Forms.Padding(4, 3, 4, 3);
            this.progressBar.Name = "progressBar";
            this.progressBar.Size = new System.Drawing.Size(521, 26);
            this.progressBar.TabIndex = 2;
            // 
            // journalRichBox
            // 
            this.journalRichBox.Anchor = ((System.Windows.Forms.AnchorStyles)((((System.Windows.Forms.AnchorStyles.Top | System.Windows.Forms.AnchorStyles.Bottom) 
            | System.Windows.Forms.AnchorStyles.Left) 
            | System.Windows.Forms.AnchorStyles.Right)));
            this.journalRichBox.BackColor = System.Drawing.SystemColors.Window;
            this.journalRichBox.Location = new System.Drawing.Point(14, 13);
            this.journalRichBox.Margin = new System.Windows.Forms.Padding(4, 3, 4, 3);
            this.journalRichBox.Name = "journalRichBox";
            this.journalRichBox.ReadOnly = true;
            this.journalRichBox.ScrollBars = System.Windows.Forms.RichTextBoxScrollBars.Vertical;
            this.journalRichBox.Size = new System.Drawing.Size(521, 218);
            this.journalRichBox.TabIndex = 3;
            this.journalRichBox.Text = "";
            // 
            // PatchForm
            // 
            this.AutoScaleDimensions = new System.Drawing.SizeF(7F, 14F);
            this.AutoScaleMode = System.Windows.Forms.AutoScaleMode.Font;
            this.ClientSize = new System.Drawing.Size(549, 272);
            this.Controls.Add(this.journalRichBox);
            this.Controls.Add(this.progressBar);
            this.Font = new System.Drawing.Font("Consolas", 9F, System.Drawing.FontStyle.Regular, System.Drawing.GraphicsUnit.Point, ((byte)(204)));
            this.Icon = ((System.Drawing.Icon)(resources.GetObject("$this.Icon")));
            this.Margin = new System.Windows.Forms.Padding(4, 3, 4, 3);
            this.MaximizeBox = false;
            this.Name = "PatchForm";
            this.StartPosition = System.Windows.Forms.FormStartPosition.CenterParent;
            this.Text = "Droute: Applying Patch";
            this.FormClosing += new System.Windows.Forms.FormClosingEventHandler(this.PatchForm_FormClosing);
            this.Shown += new System.EventHandler(this.PatchForm_Shown);
            this.ResumeLayout(false);

        }

        #endregion
        private System.Windows.Forms.ProgressBar progressBar;
        private System.Windows.Forms.RichTextBox journalRichBox;
    }
}