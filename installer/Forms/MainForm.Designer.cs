namespace Droute.Installer.Forms
{
    partial class MainForm
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
            System.ComponentModel.ComponentResourceManager resources = new System.ComponentModel.ComponentResourceManager(typeof(MainForm));
            this.tabControl = new System.Windows.Forms.TabControl();
            this.tabPage1 = new System.Windows.Forms.TabPage();
            this.openLogsButton = new System.Windows.Forms.Button();
            this.removePatchButton = new System.Windows.Forms.Button();
            this.installPatchButton = new System.Windows.Forms.Button();
            this.groupBox2 = new System.Windows.Forms.GroupBox();
            this.branchesComboBox = new System.Windows.Forms.ComboBox();
            this.label6 = new System.Windows.Forms.Label();
            this.autoRestartPatchCheckbox = new System.Windows.Forms.CheckBox();
            this.autoRestartConfigCheckbox = new System.Windows.Forms.CheckBox();
            this.groupBox1 = new System.Windows.Forms.GroupBox();
            this.resetCfgBtn = new System.Windows.Forms.Button();
            this.applyCfgButton = new System.Windows.Forms.Button();
            this.authPanel = new System.Windows.Forms.Panel();
            this.passwordTextBox = new System.Windows.Forms.TextBox();
            this.label4 = new System.Windows.Forms.Label();
            this.userTextBox = new System.Windows.Forms.TextBox();
            this.label3 = new System.Windows.Forms.Label();
            this.portNumeric = new System.Windows.Forms.NumericUpDown();
            this.label2 = new System.Windows.Forms.Label();
            this.hostTextBox = new System.Windows.Forms.TextBox();
            this.authCheckBox = new System.Windows.Forms.CheckBox();
            this.label1 = new System.Windows.Forms.Label();
            this.tabPage2 = new System.Windows.Forms.TabPage();
            this.versionLabel = new System.Windows.Forms.Label();
            this.licenseLink = new System.Windows.Forms.LinkLabel();
            this.repoLink = new System.Windows.Forms.LinkLabel();
            this.inspiredLink = new System.Windows.Forms.LinkLabel();
            this.label11 = new System.Windows.Forms.Label();
            this.label10 = new System.Windows.Forms.Label();
            this.label8 = new System.Windows.Forms.Label();
            this.label9 = new System.Windows.Forms.Label();
            this.label7 = new System.Windows.Forms.Label();
            this.pictureBox1 = new System.Windows.Forms.PictureBox();
            this.label5 = new System.Windows.Forms.Label();
            this.tabControl.SuspendLayout();
            this.tabPage1.SuspendLayout();
            this.groupBox2.SuspendLayout();
            this.groupBox1.SuspendLayout();
            this.authPanel.SuspendLayout();
            ((System.ComponentModel.ISupportInitialize)(this.portNumeric)).BeginInit();
            this.tabPage2.SuspendLayout();
            ((System.ComponentModel.ISupportInitialize)(this.pictureBox1)).BeginInit();
            this.SuspendLayout();
            // 
            // tabControl
            // 
            resources.ApplyResources(this.tabControl, "tabControl");
            this.tabControl.Controls.Add(this.tabPage1);
            this.tabControl.Controls.Add(this.tabPage2);
            this.tabControl.Name = "tabControl";
            this.tabControl.SelectedIndex = 0;
            // 
            // tabPage1
            // 
            resources.ApplyResources(this.tabPage1, "tabPage1");
            this.tabPage1.Controls.Add(this.openLogsButton);
            this.tabPage1.Controls.Add(this.removePatchButton);
            this.tabPage1.Controls.Add(this.installPatchButton);
            this.tabPage1.Controls.Add(this.groupBox2);
            this.tabPage1.Controls.Add(this.groupBox1);
            this.tabPage1.Name = "tabPage1";
            this.tabPage1.UseVisualStyleBackColor = true;
            // 
            // openLogsButton
            // 
            resources.ApplyResources(this.openLogsButton, "openLogsButton");
            this.openLogsButton.Name = "openLogsButton";
            this.openLogsButton.UseVisualStyleBackColor = true;
            this.openLogsButton.Click += new System.EventHandler(this.openLogsButton_Click);
            // 
            // removePatchButton
            // 
            resources.ApplyResources(this.removePatchButton, "removePatchButton");
            this.removePatchButton.Name = "removePatchButton";
            this.removePatchButton.UseVisualStyleBackColor = true;
            this.removePatchButton.Click += new System.EventHandler(this.removePatchButton_Click);
            // 
            // installPatchButton
            // 
            resources.ApplyResources(this.installPatchButton, "installPatchButton");
            this.installPatchButton.Name = "installPatchButton";
            this.installPatchButton.UseVisualStyleBackColor = true;
            this.installPatchButton.Click += new System.EventHandler(this.installPatchButton_Click);
            // 
            // groupBox2
            // 
            resources.ApplyResources(this.groupBox2, "groupBox2");
            this.groupBox2.Controls.Add(this.branchesComboBox);
            this.groupBox2.Controls.Add(this.label6);
            this.groupBox2.Controls.Add(this.autoRestartPatchCheckbox);
            this.groupBox2.Controls.Add(this.autoRestartConfigCheckbox);
            this.groupBox2.Name = "groupBox2";
            this.groupBox2.TabStop = false;
            // 
            // branchesComboBox
            // 
            resources.ApplyResources(this.branchesComboBox, "branchesComboBox");
            this.branchesComboBox.FormattingEnabled = true;
            this.branchesComboBox.Items.AddRange(new object[] {
            resources.GetString("branchesComboBox.Items"),
            resources.GetString("branchesComboBox.Items1"),
            resources.GetString("branchesComboBox.Items2")});
            this.branchesComboBox.Name = "branchesComboBox";
            // 
            // label6
            // 
            resources.ApplyResources(this.label6, "label6");
            this.label6.Name = "label6";
            // 
            // autoRestartPatchCheckbox
            // 
            resources.ApplyResources(this.autoRestartPatchCheckbox, "autoRestartPatchCheckbox");
            this.autoRestartPatchCheckbox.Checked = true;
            this.autoRestartPatchCheckbox.CheckState = System.Windows.Forms.CheckState.Checked;
            this.autoRestartPatchCheckbox.Name = "autoRestartPatchCheckbox";
            this.autoRestartPatchCheckbox.UseVisualStyleBackColor = true;
            this.autoRestartPatchCheckbox.CheckedChanged += new System.EventHandler(this.discordActionsCheckbox_CheckedChanged);
            // 
            // autoRestartConfigCheckbox
            // 
            resources.ApplyResources(this.autoRestartConfigCheckbox, "autoRestartConfigCheckbox");
            this.autoRestartConfigCheckbox.Checked = true;
            this.autoRestartConfigCheckbox.CheckState = System.Windows.Forms.CheckState.Checked;
            this.autoRestartConfigCheckbox.Name = "autoRestartConfigCheckbox";
            this.autoRestartConfigCheckbox.UseVisualStyleBackColor = true;
            this.autoRestartConfigCheckbox.CheckedChanged += new System.EventHandler(this.discordActionsCheckbox_CheckedChanged);
            // 
            // groupBox1
            // 
            resources.ApplyResources(this.groupBox1, "groupBox1");
            this.groupBox1.Controls.Add(this.resetCfgBtn);
            this.groupBox1.Controls.Add(this.applyCfgButton);
            this.groupBox1.Controls.Add(this.authPanel);
            this.groupBox1.Controls.Add(this.portNumeric);
            this.groupBox1.Controls.Add(this.label2);
            this.groupBox1.Controls.Add(this.hostTextBox);
            this.groupBox1.Controls.Add(this.authCheckBox);
            this.groupBox1.Controls.Add(this.label1);
            this.groupBox1.Name = "groupBox1";
            this.groupBox1.TabStop = false;
            // 
            // resetCfgBtn
            // 
            resources.ApplyResources(this.resetCfgBtn, "resetCfgBtn");
            this.resetCfgBtn.Name = "resetCfgBtn";
            this.resetCfgBtn.UseVisualStyleBackColor = true;
            this.resetCfgBtn.Click += new System.EventHandler(this.resetCfgBtn_Click);
            // 
            // applyCfgButton
            // 
            resources.ApplyResources(this.applyCfgButton, "applyCfgButton");
            this.applyCfgButton.Name = "applyCfgButton";
            this.applyCfgButton.UseVisualStyleBackColor = true;
            this.applyCfgButton.Click += new System.EventHandler(this.applyCfgButton_Click);
            // 
            // authPanel
            // 
            resources.ApplyResources(this.authPanel, "authPanel");
            this.authPanel.Controls.Add(this.passwordTextBox);
            this.authPanel.Controls.Add(this.label4);
            this.authPanel.Controls.Add(this.userTextBox);
            this.authPanel.Controls.Add(this.label3);
            this.authPanel.Name = "authPanel";
            // 
            // passwordTextBox
            // 
            resources.ApplyResources(this.passwordTextBox, "passwordTextBox");
            this.passwordTextBox.Name = "passwordTextBox";
            // 
            // label4
            // 
            resources.ApplyResources(this.label4, "label4");
            this.label4.Name = "label4";
            // 
            // userTextBox
            // 
            resources.ApplyResources(this.userTextBox, "userTextBox");
            this.userTextBox.Name = "userTextBox";
            // 
            // label3
            // 
            resources.ApplyResources(this.label3, "label3");
            this.label3.Name = "label3";
            // 
            // portNumeric
            // 
            resources.ApplyResources(this.portNumeric, "portNumeric");
            this.portNumeric.Maximum = new decimal(new int[] {
            65535,
            0,
            0,
            0});
            this.portNumeric.Name = "portNumeric";
            this.portNumeric.Value = new decimal(new int[] {
            1080,
            0,
            0,
            0});
            // 
            // label2
            // 
            resources.ApplyResources(this.label2, "label2");
            this.label2.Name = "label2";
            // 
            // hostTextBox
            // 
            resources.ApplyResources(this.hostTextBox, "hostTextBox");
            this.hostTextBox.Name = "hostTextBox";
            // 
            // authCheckBox
            // 
            resources.ApplyResources(this.authCheckBox, "authCheckBox");
            this.authCheckBox.Checked = true;
            this.authCheckBox.CheckState = System.Windows.Forms.CheckState.Checked;
            this.authCheckBox.Name = "authCheckBox";
            this.authCheckBox.UseVisualStyleBackColor = true;
            this.authCheckBox.CheckedChanged += new System.EventHandler(this.authCheckBox_CheckedChanged);
            // 
            // label1
            // 
            resources.ApplyResources(this.label1, "label1");
            this.label1.Name = "label1";
            // 
            // tabPage2
            // 
            resources.ApplyResources(this.tabPage2, "tabPage2");
            this.tabPage2.Controls.Add(this.versionLabel);
            this.tabPage2.Controls.Add(this.licenseLink);
            this.tabPage2.Controls.Add(this.repoLink);
            this.tabPage2.Controls.Add(this.inspiredLink);
            this.tabPage2.Controls.Add(this.label11);
            this.tabPage2.Controls.Add(this.label10);
            this.tabPage2.Controls.Add(this.label8);
            this.tabPage2.Controls.Add(this.label9);
            this.tabPage2.Controls.Add(this.label7);
            this.tabPage2.Controls.Add(this.pictureBox1);
            this.tabPage2.Controls.Add(this.label5);
            this.tabPage2.Name = "tabPage2";
            this.tabPage2.UseVisualStyleBackColor = true;
            // 
            // versionLabel
            // 
            resources.ApplyResources(this.versionLabel, "versionLabel");
            this.versionLabel.ForeColor = System.Drawing.Color.Gray;
            this.versionLabel.Name = "versionLabel";
            // 
            // licenseLink
            // 
            resources.ApplyResources(this.licenseLink, "licenseLink");
            this.licenseLink.ActiveLinkColor = System.Drawing.Color.FromArgb(((int)(((byte)(91)))), ((int)(((byte)(102)))), ((int)(((byte)(222)))));
            this.licenseLink.LinkColor = System.Drawing.Color.Black;
            this.licenseLink.Name = "licenseLink";
            this.licenseLink.TabStop = true;
            this.licenseLink.VisitedLinkColor = System.Drawing.Color.Purple;
            this.licenseLink.LinkClicked += new System.Windows.Forms.LinkLabelLinkClickedEventHandler(this.licenseLink_LinkClicked);
            // 
            // repoLink
            // 
            resources.ApplyResources(this.repoLink, "repoLink");
            this.repoLink.ActiveLinkColor = System.Drawing.Color.FromArgb(((int)(((byte)(91)))), ((int)(((byte)(102)))), ((int)(((byte)(222)))));
            this.repoLink.LinkColor = System.Drawing.Color.Black;
            this.repoLink.Name = "repoLink";
            this.repoLink.TabStop = true;
            this.repoLink.VisitedLinkColor = System.Drawing.Color.Purple;
            this.repoLink.LinkClicked += new System.Windows.Forms.LinkLabelLinkClickedEventHandler(this.repoLink_LinkClicked);
            // 
            // inspiredLink
            // 
            resources.ApplyResources(this.inspiredLink, "inspiredLink");
            this.inspiredLink.ActiveLinkColor = System.Drawing.Color.FromArgb(((int)(((byte)(91)))), ((int)(((byte)(102)))), ((int)(((byte)(222)))));
            this.inspiredLink.LinkColor = System.Drawing.Color.Black;
            this.inspiredLink.Name = "inspiredLink";
            this.inspiredLink.TabStop = true;
            this.inspiredLink.VisitedLinkColor = System.Drawing.Color.Purple;
            this.inspiredLink.LinkClicked += new System.Windows.Forms.LinkLabelLinkClickedEventHandler(this.inspiredLink_LinkClicked);
            // 
            // label11
            // 
            resources.ApplyResources(this.label11, "label11");
            this.label11.ForeColor = System.Drawing.Color.Black;
            this.label11.Name = "label11";
            // 
            // label10
            // 
            resources.ApplyResources(this.label10, "label10");
            this.label10.ForeColor = System.Drawing.Color.Black;
            this.label10.Name = "label10";
            // 
            // label8
            // 
            resources.ApplyResources(this.label8, "label8");
            this.label8.ForeColor = System.Drawing.Color.Black;
            this.label8.Name = "label8";
            // 
            // label9
            // 
            resources.ApplyResources(this.label9, "label9");
            this.label9.ForeColor = System.Drawing.Color.Black;
            this.label9.Name = "label9";
            // 
            // label7
            // 
            resources.ApplyResources(this.label7, "label7");
            this.label7.ForeColor = System.Drawing.Color.DimGray;
            this.label7.Name = "label7";
            // 
            // pictureBox1
            // 
            resources.ApplyResources(this.pictureBox1, "pictureBox1");
            this.pictureBox1.Name = "pictureBox1";
            this.pictureBox1.TabStop = false;
            // 
            // label5
            // 
            resources.ApplyResources(this.label5, "label5");
            this.label5.ForeColor = System.Drawing.Color.Black;
            this.label5.Name = "label5";
            // 
            // MainForm
            // 
            resources.ApplyResources(this, "$this");
            this.AutoScaleMode = System.Windows.Forms.AutoScaleMode.Font;
            this.Controls.Add(this.tabControl);
            this.FormBorderStyle = System.Windows.Forms.FormBorderStyle.FixedSingle;
            this.MaximizeBox = false;
            this.Name = "MainForm";
            this.Load += new System.EventHandler(this.MainForm_Load);
            this.tabControl.ResumeLayout(false);
            this.tabPage1.ResumeLayout(false);
            this.groupBox2.ResumeLayout(false);
            this.groupBox2.PerformLayout();
            this.groupBox1.ResumeLayout(false);
            this.groupBox1.PerformLayout();
            this.authPanel.ResumeLayout(false);
            this.authPanel.PerformLayout();
            ((System.ComponentModel.ISupportInitialize)(this.portNumeric)).EndInit();
            this.tabPage2.ResumeLayout(false);
            this.tabPage2.PerformLayout();
            ((System.ComponentModel.ISupportInitialize)(this.pictureBox1)).EndInit();
            this.ResumeLayout(false);

        }

        #endregion

        private System.Windows.Forms.TabControl tabControl;
        private System.Windows.Forms.TabPage tabPage1;
        private System.Windows.Forms.TabPage tabPage2;
        private System.Windows.Forms.GroupBox groupBox1;
        private System.Windows.Forms.Label label2;
        private System.Windows.Forms.TextBox hostTextBox;
        private System.Windows.Forms.CheckBox authCheckBox;
        private System.Windows.Forms.Label label1;
        private System.Windows.Forms.NumericUpDown portNumeric;
        private System.Windows.Forms.Panel authPanel;
        private System.Windows.Forms.TextBox userTextBox;
        private System.Windows.Forms.Label label3;
        private System.Windows.Forms.TextBox passwordTextBox;
        private System.Windows.Forms.Label label4;
        private System.Windows.Forms.GroupBox groupBox2;
        private System.Windows.Forms.Button installPatchButton;
        private System.Windows.Forms.Button removePatchButton;
        private System.Windows.Forms.Button openLogsButton;
        private System.Windows.Forms.Button applyCfgButton;
        private System.Windows.Forms.CheckBox autoRestartConfigCheckbox;
        private System.Windows.Forms.CheckBox autoRestartPatchCheckbox;
        private System.Windows.Forms.Label label5;
        private System.Windows.Forms.PictureBox pictureBox1;
        private System.Windows.Forms.Label label7;
        private System.Windows.Forms.Label label8;
        private System.Windows.Forms.Label label9;
        private System.Windows.Forms.LinkLabel repoLink;
        private System.Windows.Forms.LinkLabel licenseLink;
        private System.Windows.Forms.Label label10;
        private System.Windows.Forms.Label label11;
        private System.Windows.Forms.LinkLabel inspiredLink;
        private System.Windows.Forms.Label versionLabel;
        private System.Windows.Forms.Label label6;
        private System.Windows.Forms.ComboBox branchesComboBox;
        private System.Windows.Forms.Button resetCfgBtn;
    }
}