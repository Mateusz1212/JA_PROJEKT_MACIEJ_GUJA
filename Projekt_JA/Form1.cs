namespace Projekt_JA
{
    public partial class Form1 : Form
    {
        public Form1()
        {
            InitializeComponent();
        }

        private void button1_Click(object sender, EventArgs e)
        {

        }

        private void radioButton1_CheckedChanged(object sender, EventArgs e)
        {

        }

        private void Form1_Load(object sender, EventArgs e)
        {

        }

        private void radioButton2_CheckedChanged(object sender, EventArgs e)
        {

        }

        private void label1_Click(object sender, EventArgs e)
        {

        }

        private void radioButton4_CheckedChanged(object sender, EventArgs e)
        {
            label2.Text = "Destynacja skompresowanych plików";
            label3.Visible = false;
            button3.Visible = false;
            button2.Text = "Wybierz zipa";
        }

        private void label5_Click(object sender, EventArgs e)
        {

        }

        private void radioButton3_CheckedChanged(object sender, EventArgs e)
        {
            label2.Text = "Wybór folderu z obrazkami";
            label3.Visible = true;
            button3.Visible = true;
            button2.Text = "Wybierz folder";
        }
    }
}
