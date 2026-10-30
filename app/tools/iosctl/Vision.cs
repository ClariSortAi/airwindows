// Sampling and layout inspection adapted from Claude Code's local iosctl.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;

static class Vision {
    internal static byte[] Sample(Bitmap image) {
        // Fixed sample geometry keeps window size out of the comparison. Ignore
        // the animated mirroring/status area, as in Claude's original helper.
        byte[] result=new byte[32*64];
        for(int y=0;y<64;y++) for(int x=0;x<32;x++) {
            var p=image.GetPixel(Math.Min(image.Width-1,x*image.Width/32),Math.Min(image.Height-1,(int)((.075+.925*y/64)*image.Height)));
            result[y*32+x]=(byte)((p.R+p.G+p.B)/3);
        }
        return result;
    }
    internal static int Difference(byte[] a,byte[] b) {
        if(a==null || b==null || a.Length!=b.Length) throw new InvalidOperationException("No comparable baseline; perform one input first");
        int different=0;for(int i=0;i<a.Length;i++) if(Math.Abs(a[i]-b[i])>10)different++;
        return (int)(1000L*different/a.Length);
    }
    internal static void Save(PhoneFrame frame,string path,double scale,Rectangle region) {
        if(region.X<0 || region.Y<0 || region.Width<1 || region.Height<1 || region.Right>frame.Width || region.Bottom>frame.Height) throw new ArgumentException("Crop is outside the phone image");
        int w=Math.Max(1,(int)Math.Round(region.Width*scale)),h=Math.Max(1,(int)Math.Round(region.Height*scale));
        if(w>4096 || h>4096) throw new ArgumentException("Output image exceeds 4096 pixels per axis");
        string full=Path.GetFullPath(path);Directory.CreateDirectory(Path.GetDirectoryName(full));
        using(var output=new Bitmap(w,h)) {
            using(var g=Graphics.FromImage(output)) {
                g.InterpolationMode=System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
                g.DrawImage(frame.Image,new Rectangle(0,0,w,h),region,GraphicsUnit.Pixel);
            }
            output.Save(full,ImageFormat.Png);
        }
        var meta=new Dictionary<string,object>(frame.Meta);
        meta["output_width"]=w;meta["output_height"]=h;meta["crop_x"]=region.X;meta["crop_y"]=region.Y;
        meta["crop_width"]=region.Width;meta["crop_height"]=region.Height;
        File.WriteAllText(Path.ChangeExtension(full,".json"),Frames.Json.Serialize(meta));
    }
    static List<int[]> Runs(int[] values,int minimum,int threshold) {
        var result=new List<int[]>();int start=-1;
        for(int i=0;i<=values.Length;i++) {
            if(i<values.Length && values[i]>=threshold) { if(start<0)start=i; }
            else if(start>=0) { if(i-start>=minimum) result.Add(new[]{start,i-1});start=-1; }
        }
        return result;
    }
    internal static object Grid(Bitmap image,int threshold) {
        var columns=new int[image.Width];var rows=new int[image.Height];
        for(int y=0;y<image.Height;y++)for(int x=0;x<image.Width;x++) {
            var p=image.GetPixel(x,y);if((p.R+p.G+p.B)/3>threshold){columns[x]++;rows[y]++;}
        }
        return new { width=image.Width,height=image.Height,
            columns=Runs(columns,Math.Max(1,image.Width*3/100),Math.Max(1,image.Height/100)),
            rows=Runs(rows,Math.Max(1,image.Height*15/1000),Math.Max(1,image.Width/100)) };
    }
}
