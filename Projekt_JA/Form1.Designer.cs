namespace Projekt_JA
{
    partial class Form1
    {
        /// <summary>
        ///  Required designer variable.
        /// </summary>
        private System.ComponentModel.IContainer components = null;

        /// <summary>
        ///  Clean up any resources being used.
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
        ///  Required method for Designer support - do not modify
        ///  the contents of this method with the code editor.
        /// </summary>
        private void InitializeComponent()
        {
            button1 = new Button();
            radioButton1 = new RadioButton();
            radioButton2 = new RadioButton();
            trackBar1 = new TrackBar();
            groupBox1 = new GroupBox();
            label1 = new Label();
            button2 = new Button();
            label2 = new Label();
            radioButton3 = new RadioButton();
            radioButton4 = new RadioButton();
            label3 = new Label();
            button3 = new Button();
            label4 = new Label();
            label5 = new Label();
            label6 = new Label();
            groupBox2 = new GroupBox();
            groupBox3 = new GroupBox();
            ((System.ComponentModel.ISupportInitialize)trackBar1).BeginInit();
            groupBox1.SuspendLayout();
            groupBox2.SuspendLayout();
            groupBox3.SuspendLayout();
            SuspendLayout();
            // 
            // button1
            // 
            button1.AutoSize = true;
            button1.AutoSizeMode = AutoSizeMode.GrowAndShrink;
            button1.Location = new Point(12, 295);
            button1.Name = "button1";
            button1.Size = new Size(90, 30);
            button1.TabIndex = 0;
            button1.Text = "Kompresuj";
            button1.UseVisualStyleBackColor = true;
            button1.Click += button1_Click;
            // 
            // radioButton1
            // 
            radioButton1.AutoSize = true;
            radioButton1.Checked = true;
            radioButton1.Location = new Point(16, 26);
            radioButton1.Name = "radioButton1";
            radioButton1.Size = new Size(59, 24);
            radioButton1.TabIndex = 1;
            radioButton1.TabStop = true;
            radioButton1.Text = "C++";
            radioButton1.UseVisualStyleBackColor = true;
            radioButton1.CheckedChanged += radioButton1_CheckedChanged;
            // 
            // radioButton2
            // 
            radioButton2.AutoSize = true;
            radioButton2.Location = new Point(16, 56);
            radioButton2.Name = "radioButton2";
            radioButton2.Size = new Size(88, 24);
            radioButton2.TabIndex = 2;
            radioButton2.Text = "ASM x64";
            radioButton2.UseVisualStyleBackColor = true;
            radioButton2.CheckedChanged += radioButton2_CheckedChanged;
            // 
            // trackBar1
            // 
            trackBar1.Location = new Point(172, 216);
            trackBar1.Name = "trackBar1";
            trackBar1.Size = new Size(320, 56);
            trackBar1.TabIndex = 4;
            // 
            // groupBox1
            // 
            groupBox1.Controls.Add(radioButton1);
            groupBox1.Controls.Add(radioButton2);
            groupBox1.Location = new Point(12, 181);
            groupBox1.Name = "groupBox1";
            groupBox1.Size = new Size(152, 91);
            groupBox1.TabIndex = 5;
            groupBox1.TabStop = false;
            groupBox1.Text = "Wybór biblioteki";
            // 
            // label1
            // 
            label1.AutoSize = true;
            label1.Location = new Point(179, 190);
            label1.Name = "label1";
            label1.Size = new Size(106, 20);
            label1.TabIndex = 6;
            label1.Text = "Liczba wątków";
            label1.Click += label1_Click;
            // 
            // button2
            // 
            button2.Location = new Point(12, 26);
            button2.Name = "button2";
            button2.Size = new Size(105, 29);
            button2.TabIndex = 7;
            button2.Text = "Wybierz...";
            button2.UseVisualStyleBackColor = true;
            // 
            // label2
            // 
            label2.AutoSize = true;
            label2.Location = new Point(123, 30);
            label2.Name = "label2";
            label2.Size = new Size(190, 20);
            label2.TabIndex = 8;
            label2.Text = "Wybór folderu z obrazkami";
            label2.Click += label2_Click;
            // 
            // radioButton3
            // 
            radioButton3.AutoSize = true;
            radioButton3.Checked = true;
            radioButton3.Location = new Point(16, 26);
            radioButton3.Name = "radioButton3";
            radioButton3.Size = new Size(152, 24);
            radioButton3.TabIndex = 9;
            radioButton3.TabStop = true;
            radioButton3.Text = "Kompresuj zdjęcia";
            radioButton3.UseVisualStyleBackColor = true;
            radioButton3.CheckedChanged += radioButton3_CheckedChanged;
            // 
            // radioButton4
            // 
            radioButton4.AutoSize = true;
            radioButton4.Location = new Point(182, 26);
            radioButton4.Name = "radioButton4";
            radioButton4.Size = new Size(169, 24);
            radioButton4.TabIndex = 10;
            radioButton4.Text = "Dekompresuj zdjęcia";
            radioButton4.UseVisualStyleBackColor = true;
            radioButton4.CheckedChanged += radioButton4_CheckedChanged;
            // 
            // label3
            // 
            label3.AutoSize = true;
            label3.Location = new Point(122, 62);
            label3.Name = "label3";
            label3.Size = new Size(254, 20);
            label3.TabIndex = 12;
            label3.Text = "Destynacja skompresowanych plików";
            label3.Click += label3_Click;
            // 
            // button3
            // 
            button3.Location = new Point(12, 58);
            button3.Name = "button3";
            button3.Size = new Size(105, 29);
            button3.TabIndex = 11;
            button3.Text = "Wybierz...";
            button3.UseVisualStyleBackColor = true;
            // 
            // label4
            // 
            label4.AutoSize = true;
            label4.Location = new Point(183, 254);
            label4.Name = "label4";
            label4.Size = new Size(17, 20);
            label4.TabIndex = 13;
            label4.Text = "1";
            // 
            // label5
            // 
            label5.AutoSize = true;
            label5.Location = new Point(459, 234);
            label5.Name = "label5";
            label5.Size = new Size(25, 20);
            label5.TabIndex = 14;
            label5.Text = "64";
            label5.Click += label5_Click;
            // 
            // label6
            // 
            label6.AutoSize = true;
            label6.Location = new Point(390, 193);
            label6.Name = "label6";
            label6.Size = new Size(84, 20);
            label6.TabIndex = 15;
            label6.Text = "Wybrano: 1";
            label6.Click += label6_Click;
            // 
            // groupBox2
            // 
            groupBox2.Controls.Add(radioButton3);
            groupBox2.Controls.Add(radioButton4);
            groupBox2.Location = new Point(12, 12);
            groupBox2.Name = "groupBox2";
            groupBox2.Size = new Size(403, 61);
            groupBox2.TabIndex = 16;
            groupBox2.TabStop = false;
            groupBox2.Text = "Wybór operacji";
            // 
            // groupBox3
            // 
            groupBox3.Controls.Add(button3);
            groupBox3.Controls.Add(button2);
            groupBox3.Controls.Add(label2);
            groupBox3.Controls.Add(label3);
            groupBox3.Location = new Point(12, 79);
            groupBox3.Name = "groupBox3";
            groupBox3.Size = new Size(382, 96);
            groupBox3.TabIndex = 17;
            groupBox3.TabStop = false;
            groupBox3.Text = "Lokalizacja danych";
            // 
            // Form1
            // 
            AutoScaleDimensions = new SizeF(8F, 20F);
            AutoScaleMode = AutoScaleMode.Font;
            ClientSize = new Size(512, 353);
            Controls.Add(groupBox3);
            Controls.Add(groupBox2);
            Controls.Add(label6);
            Controls.Add(label5);
            Controls.Add(label4);
            Controls.Add(label1);
            Controls.Add(groupBox1);
            Controls.Add(trackBar1);
            Controls.Add(button1);
            MinimumSize = new Size(530, 400);
            Name = "Form1";
            Text = "Form1";
            Load += Form1_Load;
            ((System.ComponentModel.ISupportInitialize)trackBar1).EndInit();
            groupBox1.ResumeLayout(false);
            groupBox1.PerformLayout();
            groupBox2.ResumeLayout(false);
            groupBox2.PerformLayout();
            groupBox3.ResumeLayout(false);
            groupBox3.PerformLayout();
            ResumeLayout(false);
            PerformLayout();
        }

        #endregion

        private Button button1;
        private RadioButton radioButton1;
        private RadioButton radioButton2;
        private TrackBar trackBar1;
        private GroupBox groupBox1;
        private Label label1;
        private Button button2;
        private Label label2;
        private RadioButton radioButton3;
        private RadioButton radioButton4;
        private Label label3;
        private Button button3;
        private Label label4;
        private Label label5;
        private Label label6;
        private GroupBox groupBox2;
        private GroupBox groupBox3;
    }
}
