namespace Projekt_JA
{
    public partial class Form1 : Form
    {
        private bool isCompression = true;
        private string sourcePath = string.Empty;
        private string destinationPath = string.Empty;

        // Rozmiary groupBox3
        private const int GroupBox3FullHeight = 95;
        private const int GroupBox3SmallHeight = 61;

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
            // Wczytaj ikonê z pliku icon.png znajduj¹cego siê w folderze aplikacji
            string iconPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "icon.png");
            if (File.Exists(iconPath))
            {
                using Bitmap bmp = new Bitmap(iconPath);
                this.Icon = ConvertBitmapToIcon(bmp);
            }

            int processorCount = Environment.ProcessorCount;

            trackBar1.Minimum = 1;
            trackBar1.Maximum = 64;
            trackBar1.Value = Math.Min(processorCount, 64);
            trackBar1.TickFrequency = 1;

            label6.Text = $"Wybrano: {trackBar1.Value}";

            trackBar1.ValueChanged += trackBar1_ValueChanged;

            button2.Click += button2_Click;
            button3.Click += button3_Click;
        }

        /// <summary>
        /// Konwertuje Bitmap na Icon bez potrzeby zewnêtrznych bibliotek.
        /// Zachowuje kana³ alpha (przeŸroczystoœæ).
        /// </summary>
        private static Icon ConvertBitmapToIcon(Bitmap bmp)
        {
            using MemoryStream ms = new MemoryStream();

            // Nag³ówek ICO
            ms.Write(new byte[] { 0, 0 }, 0, 2);       // Reserved
            ms.Write(new byte[] { 1, 0 }, 0, 2);       // Type: ICO
            ms.Write(new byte[] { 1, 0 }, 0, 2);       // Count: 1 obraz

            // Wpis w katalogu obrazów
            int width = bmp.Width >= 256 ? 0 : bmp.Width;
            int height = bmp.Height >= 256 ? 0 : bmp.Height;

            using MemoryStream pngStream = new MemoryStream();
            bmp.Save(pngStream, System.Drawing.Imaging.ImageFormat.Png);
            byte[] pngBytes = pngStream.ToArray();
            int imageSize = pngBytes.Length;

            // Offset do danych obrazu = 6 (nag³ówek) + 16 (jeden wpis katalogu)
            int imageOffset = 6 + 16;

            ms.WriteByte((byte)width);
            ms.WriteByte((byte)height);
            ms.WriteByte(0);   // ColorCount
            ms.WriteByte(0);   // Reserved
            ms.Write(new byte[] { 1, 0 }, 0, 2);  // Planes
            ms.Write(new byte[] { 32, 0 }, 0, 2); // BitCount
            ms.Write(BitConverter.GetBytes(imageSize), 0, 4);
            ms.Write(BitConverter.GetBytes(imageOffset), 0, 4);

            // Dane obrazu (PNG w œrodku ICO – obs³ugiwane od Vista+)
            ms.Write(pngBytes, 0, pngBytes.Length);

            ms.Seek(0, SeekOrigin.Begin);
            return new Icon(ms);
        }

        private void trackBar1_ValueChanged(object sender, EventArgs e)
        {
            label6.Text = $"Wybrano: {trackBar1.Value}";
        }

        private void radioButton2_CheckedChanged(object sender, EventArgs e)
        {

        }

        private void label1_Click(object sender, EventArgs e)
        {

        }

        private void radioButton4_CheckedChanged(object sender, EventArgs e)
        {
            isCompression = false;
            label2.Text = "Wybór pliku ZIP do dekompresji";
            label3.Visible = false;
            button3.Visible = false;
            button1.Text = "Dekompresuj";
            AdjustLayout(false);
        }

        private void radioButton3_CheckedChanged(object sender, EventArgs e)
        {
            isCompression = true;
            label2.Text = "Wybór folderu z obrazkami";
            label3.Visible = true;
            button3.Visible = true;
            button1.Text = "Kompresuj";
            AdjustLayout(true);
        }

        /// <summary>
        /// Dostosowuje rozmiar groupBox3 i przesuwa kontrolki poni¿ej niego.
        /// </summary>
        private void AdjustLayout(bool fullSize)
        {
            int newHeight = fullSize ? GroupBox3FullHeight : GroupBox3SmallHeight;
            int delta = newHeight - groupBox3.Height;

            if (delta == 0) return;

            groupBox3.Height = newHeight;

            // Przesuñ wszystkie kontrolki le¿¹ce poni¿ej groupBox3
            Control[] controls = { groupBox1, label1, trackBar1, label4, label5, label6, button1 };
            foreach (var ctrl in controls)
            {
                ctrl.Top += delta;
            }

            // Dostosuj wysokoœæ formularza
            ClientSize = new Size(ClientSize.Width, ClientSize.Height + delta);
        }

        private void button2_Click(object sender, EventArgs e)
        {
            if (isCompression)
            {
                using FolderBrowserDialog folderDialog = new FolderBrowserDialog();
                folderDialog.Description = "Wybierz folder ze zdjêciami";
                if (folderDialog.ShowDialog() == DialogResult.OK)
                {
                    sourcePath = folderDialog.SelectedPath;
                    label2.Text = sourcePath;
                }
            }
            else
            {
                using OpenFileDialog openDialog = new OpenFileDialog();
                openDialog.Title = "Wybierz plik ZIP";
                openDialog.Filter = "Pliki ZIP|*.zip";
                if (openDialog.ShowDialog() == DialogResult.OK)
                {
                    sourcePath = openDialog.FileName;
                    label2.Text = sourcePath;
                }
            }
        }

        private void button3_Click(object sender, EventArgs e)
        {
            using SaveFileDialog saveDialog = new SaveFileDialog();
            saveDialog.Title = "Wybierz lokalizacjê zapisu pliku ZIP";
            saveDialog.Filter = "Pliki ZIP|*.zip";
            saveDialog.DefaultExt = "zip";
            if (saveDialog.ShowDialog() == DialogResult.OK)
            {
                destinationPath = saveDialog.FileName;
                label3.Text = destinationPath;
            }
        }

        private void label3_Click(object sender, EventArgs e)
        {

        }

        private void label2_Click(object sender, EventArgs e)
        {

        }

        private void label6_Click(object sender, EventArgs e)
        {

        }

        private void label5_Click(object sender, EventArgs e)
        {

        }
    }
}