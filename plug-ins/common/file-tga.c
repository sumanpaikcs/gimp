/* GIMP - The GNU Image Manipulation Program
 * Copyright (C) 1995 Spencer Kimball and Peter Mattis
 *
 * TrueVision Targa loading and exporting file filter for GIMP.
 * Targa code Copyright (C) 1997 Raphael FRANCOIS and Gordon Matzigkeit
 *
 * The Targa reading and writing code was written from scratch by
 * Raphael FRANCOIS <fraph@ibm.net> and Gordon Matzigkeit
 * <gord@gnu.ai.mit.edu> based on the TrueVision TGA File Format
 * Specification, Version 2.0:
 *
 *   <URL:ftp://ftp.truevision.com/pub/TGA.File.Format.Spec/>
 *
 * It does not contain any code written for other TGA file loaders.
 * Not even the RLE handling. ;)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * Modified 2007-07-20, Raphaël Quinet <raphael@gimp.org>:
 *   - Workaround for loading indexed images with full alpha channel.
 *   - Bug fix: save_image() was saving uninitialized tile data for
 *     indexed images with alpha.
 *
 * Modified August-November 2000, Nick Lamb <njl195@zepler.org.uk>
 *   - Clean-up more code, avoid structure implementation dependency,
 *   - Load more types of images reliably, reject others firmly
 *   - This is not perfect, but I think it's much better. Test please!
 *
 * Release 1.2, 1997-09-24, Gordon Matzigkeit <gord@gnu.ai.mit.edu>:
 *   - Bug fixes and source cleanups.
 *
 * Release 1.1, 1997-09-19, Gordon Matzigkeit <gord@gnu.ai.mit.edu>:
 *   - Preserve alpha channels.  For indexed images, this can only be
 *     done if there is at least one free colormap entry.
 *
 * Release 1.0, 1997-09-06, Gordon Matzigkeit <gord@gnu.ai.mit.edu>:
 *   - Handle loading all image types from the 2.0 specification.
 *   - Fix many alignment and endianness problems.
 *   - Use tiles for lower memory consumption and better speed.
 *   - Rewrite RLE code for clarity and speed.
 *   - Handle saving with RLE.
 *
 * Release 0.9, 1997-06-18, Raphael FRANCOIS <fraph@ibm.net>:
 *   - Can load 24 and 32-bit Truecolor images, with and without RLE.
 *   - Saving currently only works without RLE.
 *
 *
 * TODO:
 *   - Handle TGA images with version 2 extensions (image comment,
 *     resolution, date, ...).
 *   - GIMP stores the indexed alpha channel as a separate byte,
 *     one for each pixel.  The TGA file format spec requires that the
 *     alpha channel be stored as part of the colormap, not with each
 *     individual pixel.  This means that we have no good way of
 *     saving and loading INDEXEDA images that use alpha channel values
 *     other than 0 and 255.  Workaround implemented for loading by
 *     promoting the image to RGBA, but saving indexed TGA images with
 *     full alpha information in the coloramp is not supported yet (only
 *     one fully transparent color is allowed in INDEXEDA mode).
 */

#include "config.h"

#include <errno.h>
#include <string.h>

#include <glib/gstdio.h>

#include <libgimp/gimp.h>
#include <libgimp/gimpui.h>

#include "libgimp/stdplugins-intl.h"


#define LOAD_PROC      "file-tga-load"
#define SAVE_PROC      "file-tga-save"
#define PLUG_IN_BINARY "file-tga"
#define PLUG_IN_ROLE   "gimp-file-tga"


typedef enum
{
  ORIGIN_TOP_LEFT    = 0,
  ORIGIN_BOTTOM_LEFT = 1
} TgaOrigin;


typedef struct tga_info_struct
{
  guint8 idLength;
  guint8 colorMapType;

  guint8 imageType;
  /* Known image types. */
#define TGA_TYPE_MAPPED      1
#define TGA_TYPE_COLOR       2
#define TGA_TYPE_GRAY        3

  guint8 imageCompression;
  /* Only known compression is RLE */
#define TGA_COMP_NONE        0
#define TGA_COMP_RLE         1

  /* Color Map Specification. */
  /* We need to separately specify high and low bytes to avoid endianness
     and alignment problems. */

  guint16 colorMapIndex;
  guint16 colorMapLength;
  guint8 colorMapSize;

  /* Image Specification. */
  guint16 xOrigin;
  guint16 yOrigin;

  guint16 width;
  guint16 height;

  guint8 bpp;
  guint8 bytes;

  guint8 alphaBits;
  guint8 flipHoriz;
  guint8 flipVert;

  /* Extensions (version 2) */

/* Not all the structures described in the standard are transcribed here
   only those which seem applicable to Gimp */

  gchar   authorName[41];
  gchar   comment[324];
  guint   month, day, year, hour, minute, second;
  gchar   jobName[41];
  gchar   softwareID[41];
  guint   pixelWidth, pixelHeight;  /* write dpi? */
  gdouble gamma;
} tga_info;


typedef struct _Tga      Tga;
typedef struct _TgaClass TgaClass;

struct _Tga
{
  GimpPlugIn      parent_instance;
};

struct _TgaClass
{
  GimpPlugInClass parent_class;
};


#define TGA_TYPE  (tga_get_type ())
#define TGA (obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), TGA_TYPE, Tga))

GType                   tga_get_type         (void) G_GNUC_CONST;

static GList          * tga_query_procedures (GimpPlugIn           *plug_in);
static GimpProcedure  * tga_create_procedure (GimpPlugIn           *plug_in,
                                               const gchar          *name);

static GimpValueArray * tga_load             (GimpProcedure        *procedure,
                                              GimpRunMode           run_mode,
                                              GFile                *file,
                                              const GimpValueArray *args,
                                              gpointer              run_data);
static GimpValueArray * tga_save             (GimpProcedure        *procedure,
                                              GimpRunMode           run_mode,
                                              GimpImage            *image,
                                              gint                  n_drawables,
                                              GimpDrawable        **drawables,
                                              GFile                *file,
                                              const GimpValueArray *args,
                                              gpointer              run_data);

static GimpImage      * load_image           (GFile                *file,
                                              GError              **error);
static gboolean         save_image           (GFile                *file,
                                              GimpImage            *image,
                                              GimpDrawable         *drawable,
                                              GObject              *config,
                                              GError              **error);

static gboolean         save_dialog          (GimpProcedure        *procedure,
                                              GObject              *config);

static GimpImage      * ReadImage            (FILE                 *fp,
                                              tga_info             *info,
                                              GFile                *file);


G_DEFINE_TYPE (Tga, tga, GIMP_TYPE_PLUG_IN)

GIMP_MAIN (TGA_TYPE)


/* TRUEVISION-XFILE magic signature string */
static guchar magic[18] =
{
  0x54, 0x52, 0x55, 0x45, 0x56, 0x49, 0x53, 0x49, 0x4f,
  0x4e, 0x2d, 0x58, 0x46, 0x49, 0x4c, 0x45, 0x2e, 0x0
};


static void
tga_class_init (TgaClass *klass)
{
  GimpPlugInClass *plug_in_class = GIMP_PLUG_IN_CLASS (klass);

  plug_in_class->query_procedures = tga_query_procedures;
  plug_in_class->create_procedure = tga_create_procedure;
}

static void
tga_init (Tga *tga)
{
}

static GList *
tga_query_procedures (GimpPlugIn *plug_in)
{
  GList *list = NULL;

  list = g_list_append (list, g_strdup (LOAD_PROC));
  list = g_list_append (list, g_strdup (SAVE_PROC));

  return list;
}

static GimpProcedure *
tga_create_procedure (GimpPlugIn  *plug_in,
                      const gchar *name)
{
  GimpProcedure *procedure = NULL;

  if (! strcmp (name, LOAD_PROC))
    {
      procedure = gimp_load_procedure_new (plug_in, name,
                                           GIMP_PDB_PROC_TYPE_PLUGIN,
                                           tga_load, NULL, NULL);

      gimp_procedure_set_menu_label (procedure, N_("TarGA image"));

      gimp_procedure_set_documentation (procedure,
                                        "Loads files of Targa file format",
                                        "FIXME: write help for tga_load",
                                        name);
      gimp_procedure_set_attribution (procedure,
                                      "Raphael FRANCOIS, Gordon Matzigkeit",
                                      "Raphael FRANCOIS, Gordon Matzigkeit",
                                      "1997,2000,2007");

      gimp_file_procedure_set_mime_types (GIMP_FILE_PROCEDURE (procedure),
                                          "image/x-tga");
      gimp_file_procedure_set_extensions (GIMP_FILE_PROCEDURE (procedure),
                                          "tga,vda,icb,vst");
      gimp_file_procedure_set_magics (GIMP_FILE_PROCEDURE (procedure),
                                      "-18&,string,TRUEVISION-XFILE.,-1,byte,0");
    }
  else if (! strcmp (name, SAVE_PROC))
    {
      procedure = gimp_save_procedure_new (plug_in, name,
                                           GIMP_PDB_PROC_TYPE_PLUGIN,
                                           tga_save, NULL, NULL);

      gimp_procedure_set_image_types (procedure, "*");

      gimp_procedure_set_menu_label (procedure, N_("TarGA image"));

      gimp_procedure_set_documentation (procedure,
                                        "Exports files in the Targa file format",
                                        "FIXME: write help for tga_save",
                                        name);
      gimp_procedure_set_attribution (procedure,
                                      "Raphael FRANCOIS, Gordon Matzigkeit",
                                      "Raphael FRANCOIS, Gordon Matzigkeit",
                                      "1997,2000");

      gimp_file_procedure_set_mime_types (GIMP_FILE_PROCEDURE (procedure),
                                          "image/x-tga");
      gimp_file_procedure_set_extensions (GIMP_FILE_PROCEDURE (procedure),
                                          "tga");

      GIMP_PROC_ARG_BOOLEAN (procedure, "rle",
                             "RLE",
                             "Use RLE compression",
                             TRUE,
                             G_PARAM_READWRITE);

      GIMP_PROC_ARG_INT (procedure, "origin",
                         "Origin",
                         "Image origin (0 = top-left, 1 = bottom-left)",
                         0, 1, ORIGIN_BOTTOM_LEFT,
                         G_PARAM_READWRITE);
    }

  return procedure;
}

static GimpValueArray *
tga_load (GimpProcedure        *procedure,
          GimpRunMode           run_mode,
          GFile                *file,
          const GimpValueArray *args,
          gpointer              run_data)
{
  GimpValueArray *return_vals;
  GimpImage      *image;
  GError         *error = NULL;

  INIT_I18N ();
  gegl_init (NULL, NULL);

  image = load_image (file, &error);

  if (! image)
    return gimp_procedure_new_return_values (procedure,
                                             GIMP_PDB_EXECUTION_ERROR,
                                             error);

  return_vals = gimp_procedure_new_return_values (procedure,
                                                  GIMP_PDB_SUCCESS,
                                                  NULL);

  GIMP_VALUES_SET_IMAGE (return_vals, 1, image);

  return return_vals;
}

static GimpValueArray *
tga_save (GimpProcedure        *procedure,
          GimpRunMode           run_mode,
          GimpImage            *image,
          gint                  n_drawables,
          GimpDrawable        **drawables,
          GFile                *file,
          const GimpValueArray *args,
          gpointer              run_data)
{
  GimpProcedureConfig *config;
  GimpPDBStatusType    status = GIMP_PDB_SUCCESS;
  GimpExportReturn     export = GIMP_EXPORT_CANCEL;
  GError              *error  = NULL;

  INIT_I18N ();
  gegl_init (NULL, NULL);

  config = gimp_procedure_create_config (procedure);
  gimp_procedure_config_begin_run (config, image, run_mode, args);

  switch (run_mode)
    {
    case GIMP_RUN_INTERACTIVE:
    case GIMP_RUN_WITH_LAST_VALS:
      gimp_ui_init (PLUG_IN_BINARY);

      export = gimp_export_image (&image, &n_drawables, &drawables, "TGA",
                                  GIMP_EXPORT_CAN_HANDLE_RGB     |
                                  GIMP_EXPORT_CAN_HANDLE_GRAY    |
                                  GIMP_EXPORT_CAN_HANDLE_INDEXED |
                                  GIMP_EXPORT_CAN_HANDLE_ALPHA);

      if (export == GIMP_EXPORT_CANCEL)
        return gimp_procedure_new_return_values (procedure,
                                                 GIMP_PDB_CANCEL,
                                                 NULL);
      break;

    default:
      break;
    }

  if (n_drawables != 1)
    {
      g_set_error (&error, G_FILE_ERROR, 0,
                   _("TGA format does not support multiple layers."));

      return gimp_procedure_new_return_values (procedure,
                                               GIMP_PDB_CALLING_ERROR,
                                               error);
    }

  if (run_mode == GIMP_RUN_INTERACTIVE)
    {
      if (! save_dialog (procedure, G_OBJECT (config)))
        status = GIMP_PDB_CANCEL;
    }

  if (status == GIMP_PDB_SUCCESS)
    {
      if (! save_image (file, image, drawables[0], G_OBJECT (config),
                        &error))
        {
          status = GIMP_PDB_EXECUTION_ERROR;
        }
    }

  gimp_procedure_config_end_run (config, status);
  g_object_unref (config);

  if (export == GIMP_EXPORT_EXPORT)
    {
      gimp_image_delete (image);
      g_free (drawables);
    }

  return gimp_procedure_new_return_values (procedure, status, error);
}

static GimpImage *
load_image (GFile   *file,
            GError **error)
{
  FILE      *fp;
  tga_info   info;
  guchar     header[18];
  guchar     footer[26];
  guchar     extension[495];
  long       offset;
  GimpImage *image = NULL;

  gimp_progress_init_printf (_("Opening '%s'"),
                             gimp_file_get_utf8_name (file));

  fp = g_fopen (g_file_peek_path (file), "rb");

  if (! fp)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   _("Could not open '%s' for reading: %s"),
                   gimp_file_get_utf8_name (file), g_strerror (errno));
      return NULL;
    }

  /* Is file big enough for a footer? */
  if (!fseek (fp, -26L, SEEK_END))
    {
      if (fread (footer, sizeof (footer), 1, fp) != 1)
        {
          g_message (_("Cannot read footer from '%s'"),
                     gimp_file_get_utf8_name (file));
          fclose (fp);
          return NULL;
        }
      else if (memcmp (footer + 8, magic, sizeof (magic)) == 0)
        {
          /* Check the signature. */

          offset = (footer[0]          +
                    footer[1] * 256L   +
                    footer[2] * 65536L +
                    footer[3] * 16777216L);

          if (offset != 0)
            {
              if (fseek (fp, offset, SEEK_SET) ||
                  fread (extension, sizeof (extension), 1, fp) != 1)
                {
                  g_message (_("Cannot read extension from '%s'"),
                             gimp_file_get_utf8_name (file));
                  fclose (fp);
                  return NULL;
                }
              /* Eventually actually handle version 2 TGA here */
            }
        }
    }

  if (fseek (fp, 0, SEEK_SET) ||
      fread (header, sizeof (header), 1, fp) != 1)
    {
      g_message (_("Cannot read header from '%s'"),
                 gimp_file_get_utf8_name (file));
      fclose (fp);
      return NULL;
    }

  switch (header[2])
    {
    case 1:
      info.imageType        = TGA_TYPE_MAPPED;
      info.imageCompression = TGA_COMP_NONE;
      break;
    case 2:
      info.imageType        = TGA_TYPE_COLOR;
      info.imageCompression = TGA_COMP_NONE;
      break;
    case 3:
      info.imageType        = TGA_TYPE_GRAY;
      info.imageCompression = TGA_COMP_NONE;
      break;

    case 9:
      info.imageType        = TGA_TYPE_MAPPED;
      info.imageCompression = TGA_COMP_RLE;
      break;
    case 10:
      info.imageType        = TGA_TYPE_COLOR;
      info.imageCompression = TGA_COMP_RLE;
      break;
    case 11:
      info.imageType        = TGA_TYPE_GRAY;
      info.imageCompression = TGA_COMP_RLE;
      break;

    default:
      info.imageType = 0;
    }

  info.idLength     = header[0];
  info.colorMapType = header[1];

  info.colorMapIndex  = header[3] + header[4] * 256;
  info.colorMapLength = header[5] + header[6] * 256;
  info.colorMapSize   = header[7];

  info.xOrigin = header[8]  + header[9] * 256;
  info.yOrigin = header[10] + header[11] * 256;
  info.width   = header[12] + header[13] * 256;
  info.height  = header[14] + header[15] * 256;

  info.bpp       = header[16];
  info.bytes     = (info.bpp + 7) / 8;
  info.alphaBits = header[17] & 0x0f; /* Just the low 4 bits */
  info.flipHoriz = (header[17] & 0x10) ? 1 : 0;
  info.flipVert  = (header[17] & 0x20) ? 0 : 1;

  /* hack to handle some existing files with incorrect headers, see bug #306675 */
  if (info.alphaBits == info.bpp)
    info.alphaBits = 0;

  /* hack to handle yet another flavor of incorrect headers, see bug #540969 */
  if (info.alphaBits == 0)
    {
      if (info.imageType == TGA_TYPE_MAPPED && info.colorMapSize == 32)
        info.alphaBits = 8;

      if (info.imageType == TGA_TYPE_COLOR && info.bpp == 32)
        info.alphaBits = 8;

      if (info.imageType == TGA_TYPE_GRAY && info.bpp == 16)
        info.alphaBits = 8;
    }

  switch (info.imageType)
    {
      case TGA_TYPE_MAPPED:
        if (info.bpp != 8)
          {
            g_message ("Unhandled sub-format in '%s' (type = %u, bpp = %u)",
                       gimp_file_get_utf8_name (file),
                       info.imageType, info.bpp);
            fclose (fp);
            return NULL;
          }
        break;
      case TGA_TYPE_COLOR:
        if ((info.bpp != 15 && info.bpp != 16 &&
             info.bpp != 24 && info.bpp != 32)      ||
            ((info.bpp == 15 || info.bpp == 24) &&
             info.alphaBits != 0)                   ||
            (info.bpp == 16 && info.alphaBits != 1 &&
             info.alphaBits != 0)                   ||
            (info.bpp == 32 && info.alphaBits != 8))
          {
            g_message ("Unhandled sub-format in '%s' (type = %u, bpp = %u, alpha = %u)",
                       gimp_file_get_utf8_name (file),
                       info.imageType, info.bpp, info.alphaBits);
            fclose (fp);
            return NULL;
          }
        break;
      case TGA_TYPE_GRAY:
        if (info.bpp != 8 &&
            (info.alphaBits != 8 || (info.bpp != 16 && info.bpp != 15)))
          {
            g_message ("Unhandled sub-format in '%s' (type = %u, bpp = %u)",
                       gimp_file_get_utf8_name (file),
                       info.imageType, info.bpp);
            fclose (fp);
            return NULL;
          }
        break;

      default:
        g_message ("Unknown image type %u for '%s'",
                   info.imageType, gimp_file_get_utf8_name (file));
        fclose (fp);
        return NULL;
    }

  /* Plausible but unhandled formats */
  if (info.bytes * 8 != info.bpp && info.bpp != 15)
    {
      g_message ("Unhandled sub-format in '%s' (type = %u, bpp = %u)",
                 gimp_file_get_utf8_name (file),
                 info.imageType, info.bpp);
      fclose (fp);
      return NULL;
    }

  /* Check that we have a color map only when we need it. */
  if (info.imageType == TGA_TYPE_MAPPED && info.colorMapType != 1)
    {
      g_message ("Indexed image has invalid color map type %u",
                 info.colorMapType);
      fclose (fp);
      return NULL;
    }
  else if (info.imageType != TGA_TYPE_MAPPED && info.colorMapType != 0)
    {
      g_message ("Non-indexed image has invalid color map type %u",
                 info.colorMapType);
      fclose (fp);
      return NULL;
    }

  /* Skip the image ID field. */
  if (info.idLength && fseek (fp, info.idLength, SEEK_CUR))
    {
      g_message ("File '%s' is truncated or corrupted",
                 gimp_file_get_utf8_name (file));
      fclose (fp);
      return NULL;
    }

  image = ReadImage (fp, &info, file);

  fclose (fp);

  return image;
}

static void
rle_write (FILE   *fp,
           guchar *buf,
           guint   width,
           guint   bytes)
{
  gint    repeat = 0;
  gint    direct = 0;
  guchar *from   = buf;
  guint   x;

  for (x = 1; x < width; ++x)
    {
      if (memcmp (buf, buf + bytes, bytes))
        {
          /* next pixel is different */
          if (repeat)
            {
              putc (128 + repeat, fp);
              fwrite (from, bytes, 1, fp);
              from = buf + bytes; /* point to first different pixel */
              repeat = 0;
              direct = 0;
            }
          else
            {
              direct += 1;
            }
        }
      else
        {
          /* next pixel is the same */
          if (direct)
            {
              putc (direct - 1, fp);
              fwrite (from, bytes, direct, fp);
              from = buf; /* point to first identical pixel */
              direct = 0;
              repeat = 1;
            }
          else
            {
              repeat += 1;
            }
        }

      if (repeat == 128)
        {
          putc (255, fp);
          fwrite (from, bytes, 1, fp);
          from = buf + bytes;
          direct = 0;
          repeat = 0;
        }
      else if (direct == 128)
        {
          putc (127, fp);
          fwrite (from, bytes, direct, fp);
          from = buf+ bytes;
          direct = 0;
          repeat = 0;
        }

      buf += bytes;
    }

  if (repeat > 0)
    {
      putc (128 + repeat, fp);
      fwrite (from, bytes, 1, fp);
    }
  else
    {
      putc (direct, fp);
      fwrite (from, bytes, direct + 1, fp);
    }
}

static gint
rle_read (FILE     *fp,
          guchar   *buf,
          tga_info *info)
{
  static gint   repeat = 0;
  static gint   direct = 0;
  static guchar sample[4];
  gint head;
  gint x, k;

  for (x = 0; x < info->width; x++)
    {
      if (repeat == 0 && direct == 0)
        {
          head = getc (fp);

          if (head == EOF)
            {
              return EOF;
            }
          else if (head >= 128)
            {
              repeat = head - 127;

              if (fread (sample, info->bytes, 1, fp) < 1)
                return EOF;
            }
          else
            {
              direct = head + 1;
            }
        }

      if (repeat > 0)
        {
          for (k = 0; k < info->bytes; ++k)
            {
              buf[k] = sample[k];
            }

          repeat--;
        }
      else /* direct > 0 */
        {
          if (fread (buf, info->bytes, 1, fp) < 1)
            return EOF;

          direct--;
        }

      buf += info->bytes;
    }

  return 0;
}

static void
flip_line (guchar   *buf,
           tga_info *info)
{
  guchar  temp;
  guchar *alt;
  gint    x, s;

  alt = buf + (info->bytes * (info->width - 1));

  for (x = 0; x * 2 < info->width; x++)
    {
      for (s = 0; s < info->bytes; ++s)
        {
          temp = buf[s];
          buf[s] = alt[s];
          alt[s] = temp;
        }

      buf += info->bytes;
      alt -= info->bytes;
    }
}

/* Some people write 16-bit RGB TGA files. The spec would probably
   allow 27-bit RGB too, for what it's worth, but I won't fix that
   unless someone actually provides an existence proof */

static void
upsample (guchar       *dest,
          const guchar *src,
          guint         width,
          guint         bytes,
          guint         alpha)
{
  guint x;

  for (x = 0; x < width; x++)
    {
      dest[0] =  ((src[1] << 1) & 0xf8);
      dest[0] += (dest[0] >> 5);

      dest[1] =  ((src[0] & 0xe0) >> 2) + ((src[1] & 0x03) << 6);
      dest[1] += (dest[1] >> 5);

      dest[2] =  ((src[0] << 3) & 0xf8);
      dest[2] += (dest[2] >> 5);

      if (alpha)
        {
          dest[3] = (src[1] & 0x80) ? 255 : 0;
          dest += 4;
        }
      else
        {
          dest += 3;
        }

      src += bytes;
    }
}

static void
bgr2rgb (guchar       *dest,
         const guchar *src,
         guint         width,
         guint         bytes,
         guint         alpha)
{
  guint x;

  if (alpha)
    {
      for (x = 0; x < width; x++)
        {
          *(dest++) = src[2];
          *(dest++) = src[1];
          *(dest++) = src[0];
          *(dest++) = src[3];

          src += bytes;
        }
    }
  else
    {
      for (x = 0; x < width; x++)
        {
          *(dest++) = src[2];
          *(dest++) = src[1];
          *(dest++) = src[0];

          src += bytes;
        }
    }
}

static void
apply_colormap (guchar       *dest,
                const guchar *src,
                guint         width,
                const guchar *cmap,
                gboolean      alpha,
                guint16       index)
{
  guint x;

  if (alpha)
    {
      for (x = 0; x < width; x++)
        {
          *(dest++) = cmap[(*src - index) * 4];
          *(dest++) = cmap[(*src - index) * 4 + 1];
          *(dest++) = cmap[(*src - index) * 4 + 2];
          *(dest++) = cmap[(*src - index) * 4 + 3];

          src++;
        }
    }
  else
    {
      for (x = 0; x < width; x++)
        {
          *(dest++) = cmap[(*src - index) * 3];
          *(dest++) = cmap[(*src - index) * 3 + 1];
          *(dest++) = cmap[(*src - index) * 3 + 2];

          src++;
        }
    }
}

static void
apply_index (guchar       *dest,
             const guchar *src,
             guint         width,
             guint16       index)
{
  guint x;

  for (x = 0; x < width; x++)
    {
      *(dest++) = *(src++) - index;
    }
}

static void
read_line (FILE         *fp,
           guchar       *row,
           guchar       *buf,
           tga_info     *info,
           gint          bpp,
           const guchar *convert_cmap)
{
  if (info->imageCompression == TGA_COMP_RLE)
    {
      rle_read (fp, buf, info);
    }
  else
    {
      fread (buf, info->bytes, info->width, fp);
    }

  if (info->flipHoriz)
    {
      flip_line (buf, info);
    }

  if (info->imageType == TGA_TYPE_COLOR)
    {
      if (info->bpp == 16 || info->bpp == 15)
        {
          upsample (row, buf, info->width, info->bytes, info->alphaBits);
        }
      else
        {
          bgr2rgb (row, buf, info->width, info->bytes, info->alphaBits);
        }
    }
  else if (convert_cmap)
    {
      gboolean has_alpha = (info->alphaBits > 0);

      apply_colormap (row, buf, info->width, convert_cmap, has_alpha,
                      info->colorMapIndex);
    }
  else if (info->imageType == TGA_TYPE_MAPPED)
    {
      g_assert (bpp == 1);

      apply_index (row, buf, info->width, info->colorMapIndex);
    }
  else
    {
      memcpy (row, buf, info->width * bpp);
    }
}

static GimpImage *
ReadImage (FILE     *fp,
           tga_info *info,
           GFile    *file)
{
  GimpImage         *image;
  GimpLayer         *layer;
  GeglBuffer        *buffer;
  guchar            *data, *buf, *row;
  GimpImageType      dtype = 0;
  GimpImageBaseType  itype = 0;
  gint               bpp;
  gint               i, y;
  gint               max_tileheight, tileheight;
  guint              cmap_bytes   = 0;
  guchar            *tga_cmap     = NULL;
  guchar            *gimp_cmap    = NULL;
  guchar            *convert_cmap = NULL;

  switch (info->imageType)
    {
    case TGA_TYPE_MAPPED:
      cmap_bytes = (info->colorMapSize + 7 ) / 8;
      tga_cmap = g_new (guchar, info->colorMapLength * cmap_bytes);

      if (info->colorMapSize > 24)
        {
          /* indexed + full alpha => promoted to RGBA */
          itype = GIMP_RGB;
          dtype = GIMP_RGBA_IMAGE;
          convert_cmap = g_new (guchar, info->colorMapLength * 4);
        }
      else if (info->colorMapIndex + info->colorMapLength > 256)
        {
          /* more than 256 colormap entries => promoted to RGB */
          itype = GIMP_RGB;
          dtype = GIMP_RGB_IMAGE;
          convert_cmap = g_new (guchar, info->colorMapLength * 3);
        }
      else if (info->alphaBits > 0)
        {
          /* if alpha exists here, promote to RGB */
          itype = GIMP_RGB;
          dtype = GIMP_RGBA_IMAGE;
          convert_cmap = g_new (guchar, info->colorMapLength * 4);
        }
      else
        {
          itype = GIMP_INDEXED;
          dtype = GIMP_INDEXED_IMAGE;
          gimp_cmap = g_new (guchar, info->colorMapLength * 3);
        }
      break;

    case TGA_TYPE_GRAY:
      itype = GIMP_GRAY;

      if (info->alphaBits)
        dtype = GIMP_GRAYA_IMAGE;
      else
        dtype = GIMP_GRAY_IMAGE;
      break;

    case TGA_TYPE_COLOR:
      itype = GIMP_RGB;

      if (info->alphaBits)
        dtype = GIMP_RGBA_IMAGE;
      else
        dtype = GIMP_RGB_IMAGE;
      break;
    }

  /* Handle colormap */

  if (info->imageType == TGA_TYPE_MAPPED)
    {
      if (cmap_bytes <= 4 &&
          fread (tga_cmap, info->colorMapLength * cmap_bytes, 1, fp) == 1)
        {
          if (convert_cmap)
            {
              if (info->colorMapSize == 32)
                bgr2rgb (convert_cmap, tga_cmap,
                         info->colorMapLength, cmap_bytes, 1);
              else if (info->colorMapSize == 24)
                bgr2rgb (convert_cmap, tga_cmap,
                         info->colorMapLength, cmap_bytes, 0);
              else if (info->colorMapSize == 16 || info->colorMapSize == 15)
                upsample (convert_cmap, tga_cmap,
                          info->colorMapLength, cmap_bytes, info->alphaBits);
              else
                {
                  g_message ("Unsupported colormap depth: %u",
                             info->colorMapSize);
                  return NULL;
                }
            }
          else
            {
              if (info->colorMapSize == 24)
                bgr2rgb (gimp_cmap, tga_cmap,
                         info->colorMapLength, cmap_bytes, 0);
              else if (info->colorMapSize == 16 || info->colorMapSize == 15)
                upsample (gimp_cmap, tga_cmap,
                          info->colorMapLength, cmap_bytes, info->alphaBits);
              else
                {
                  g_message ("Unsupported colormap depth: %u",
                             info->colorMapSize);
                  return NULL;
                }
            }
        }
      else
        {
          g_message ("File '%s' is truncated or corrupted",
                     gimp_file_get_utf8_name (file));
          return NULL;
        }
    }

  image = gimp_image_new (info->width, info->height, itype);
  gimp_image_set_file (image, file);

  if (gimp_cmap)
    gimp_image_set_colormap (image, gimp_cmap, info->colorMapLength);

  layer = gimp_layer_new (image,
                          _("Background"),
                          info->width, info->height,
                          dtype,
                          100,
                          gimp_image_get_default_new_layer_mode (image));

  gimp_image_insert_layer (image, layer, NULL, 0);

  buffer = gimp_drawable_get_buffer (GIMP_DRAWABLE (layer));

  bpp = gimp_drawable_get_bpp (GIMP_DRAWABLE (layer));

  /* Allocate the data. */
  max_tileheight = gimp_tile_height ();
  data = g_new (guchar, info->width * max_tileheight * bpp);
  buf  = g_new (guchar, info->width * info->bytes);

  if (info->flipVert)
    {
      for (i = 0; i < info->height; i += tileheight)
        {
          tileheight = i ? max_tileheight : (info->height % max_tileheight);
          if (tileheight == 0)
            tileheight = max_tileheight;

          for (y = 1; y <= tileheight; ++y)
            {
              row = data + (info->width * bpp * (tileheight - y));
              read_line (fp, row, buf, info, bpp, convert_cmap);
            }

          gegl_buffer_set (buffer,
                           GEGL_RECTANGLE (0, info->height - i - tileheight,
                                           info->width, tileheight), 0,
                           NULL, data, GEGL_AUTO_ROWSTRIDE);

          gimp_progress_update ((gdouble) (i + tileheight) /
                                (gdouble) info->height);
        }
    }
  else
    {
      for (i = 0; i < info->height; i += max_tileheight)
        {
          tileheight = MIN (max_tileheight, info->height - i);

          for (y = 0; y < tileheight; ++y)
            {
              row= data + (info->width * bpp * y);
              read_line (fp, row, buf, info, bpp, convert_cmap);
            }

          gegl_buffer_set (buffer,
                           GEGL_RECTANGLE (0, i, info->width, tileheight), 0,
                           NULL, data, GEGL_AUTO_ROWSTRIDE);

          gimp_progress_update ((gdouble) (i + tileheight) /
                                (gdouble) info->height);
        }
    }

  g_free (data);
  g_free (buf);

  g_free (convert_cmap);
  g_free (gimp_cmap);
  g_free (tga_cmap);

  g_object_unref (buffer);

  gimp_progress_update (1.0);

  return image;
}


static gboolean
save_image (GFile         *file,
            GimpImage     *image,
            GimpDrawable  *drawable,
            GObject       *config,
            GError       **error)
{
  GeglBuffer    *buffer;
  const Babl    *format = NULL;
  GimpImageType  dtype;
  gint           width;
  gint           height;
  FILE          *fp;
  gint           out_bpp = 0;
  gboolean       status  = TRUE;
  gint           i, row;
  guchar         header[18];
  guchar         footer[26];
  guchar        *pixels;
  guchar        *data;
  gint           num_colors;
  guchar        *gimp_cmap = NULL;
  gboolean       rle;
  TgaOrigin      origin;

  g_object_get (config,
                "rle",    &rle,
                "origin", &origin,
                NULL);

  buffer = gimp_drawable_get_buffer (drawable);

  dtype = gimp_drawable_type (drawable);

  width  = gegl_buffer_get_width  (buffer);
  height = gegl_buffer_get_height (buffer);

  gimp_progress_init_printf (_("Exporting '%s'"),
                             gimp_file_get_utf8_name (file));

  fp = g_fopen (g_file_peek_path (file), "wb");

  if (! fp)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   _("Could not open '%s' for writing: %s"),
                   gimp_file_get_utf8_name (file), g_strerror (errno));
      return FALSE;
    }

  header[0] = 0; /* No image identifier / description */

  if (dtype == GIMP_INDEXED_IMAGE)
    {
      gimp_cmap = gimp_image_get_colormap (image, &num_colors);

      header[1] = 1; /* cmap type */
      header[2] = rle ? 9 : 1;
      header[3] = header[4] = 0; /* no offset */
      header[5] = num_colors % 256;
      header[6] = num_colors / 256;
      header[7] = 24; /* cmap size / bits */
    }
  else if (dtype == GIMP_INDEXEDA_IMAGE)
    {
      gimp_cmap = gimp_image_get_colormap (image, &num_colors);

      header[1] = 1; /* cmap type */
      header[2] = rle ? 9 : 1;
      header[3] = header[4] = 0; /* no offset */
      header[5] = (num_colors + 1) % 256;
      header[6] = (num_colors + 1) / 256;
      header[7] = 32; /* cmap size / bits */
    }
  else
    {
      header[1]= 0;

      if (dtype == GIMP_RGB_IMAGE || dtype == GIMP_RGBA_IMAGE)
        {
          header[2]= rle ? 10 : 2;
        }
      else
        {
          header[2]= rle ? 11 : 3;
        }

      header[3] = header[4] = header[5] = header[6] = header[7] = 0;
    }

  header[8]  = header[9] = 0;                           /* xorigin */
  header[10] = origin ? 0 : (height % 256);      /* yorigin */
  header[11] = origin ? 0 : (height / 256);      /* yorigin */


  header[12] = width % 256;
  header[13] = width / 256;

  header[14] = height % 256;
  header[15] = height / 256;

  switch (dtype)
    {
    case GIMP_INDEXED_IMAGE:
    case GIMP_INDEXEDA_IMAGE:
      format  = NULL;
      out_bpp = 1;
      header[16] = 8; /* bpp */
      header[17] = origin ? 0 : 0x20; /* alpha + orientation */
      break;

    case GIMP_GRAY_IMAGE:
      format  = babl_format ("Y' u8");
      out_bpp = 1;
      header[16] = 8; /* bpp */
      header[17] = origin ? 0 : 0x20; /* alpha + orientation */
      break;

    case GIMP_GRAYA_IMAGE:
      format  = babl_format ("Y'A u8");
      out_bpp = 2;
      header[16] = 16; /* bpp */
      header[17] = origin ? 8 : 0x28; /* alpha + orientation */
      break;

    case GIMP_RGB_IMAGE:
      format  = babl_format ("R'G'B' u8");
      out_bpp = 3;
      header[16] = 24; /* bpp */
      header[17] = origin ? 0 : 0x20; /* alpha + orientation */
      break;

    case GIMP_RGBA_IMAGE:
      format  = babl_format ("R'G'B'A u8");
      out_bpp = 4;
      header[16] = 32; /* bpp */
      header[17] = origin ? 8 : 0x28; /* alpha + orientation */
      break;
    }

  /* write header to front of file */
  fwrite (header, sizeof (header), 1, fp);

  if (dtype == GIMP_INDEXED_IMAGE)
    {
      /* write out palette */
      for (i = 0; i < num_colors; ++i)
        {
          fputc (gimp_cmap[(i * 3) + 2], fp);
          fputc (gimp_cmap[(i * 3) + 1], fp);
          fputc (gimp_cmap[(i * 3) + 0], fp);
        }
    }
  else if (dtype == GIMP_INDEXEDA_IMAGE)
    {
      /* write out palette */
      for (i = 0; i < num_colors; ++i)
        {
          fputc (gimp_cmap[(i * 3) + 2], fp);
          fputc (gimp_cmap[(i * 3) + 1], fp);
          fputc (gimp_cmap[(i * 3) + 0], fp);
          fputc (255, fp);
        }

      fputc (0, fp);
      fputc (0, fp);
      fputc (0, fp);
      fputc (0, fp);
    }

  pixels = g_new (guchar, width * out_bpp);
  data   = g_new (guchar, width * out_bpp);

  for (row = 0; row < height; ++row)
    {
      if (origin)
        {
          gegl_buffer_get (buffer,
                           GEGL_RECTANGLE (0, height - (row + 1), width, 1), 1.0,
                           format, pixels,
                           GEGL_AUTO_ROWSTRIDE, GEGL_ABYSS_NONE);
        }
      else
        {
          gegl_buffer_get (buffer,
                           GEGL_RECTANGLE (0, row, width, 1), 1.0,
                           format, pixels,
                           GEGL_AUTO_ROWSTRIDE, GEGL_ABYSS_NONE);
        }

      if (dtype == GIMP_RGB_IMAGE)
        {
          bgr2rgb (data, pixels, width, out_bpp, 0);
        }
      else if (dtype == GIMP_RGBA_IMAGE)
        {
          bgr2rgb (data, pixels, width, out_bpp, 1);
        }
      else if (dtype == GIMP_INDEXEDA_IMAGE)
        {
          for (i = 0; i < width; ++i)
            {
              if (pixels[i * 2 + 1] > 127)
                data[i] = pixels[i * 2];
              else
                data[i] = num_colors;
            }
        }
      else
        {
          memcpy (data, pixels, width * out_bpp);
        }

      if (rle)
        {
          rle_write (fp, data, width, out_bpp);
        }
      else
        {
          fwrite (data, width * out_bpp, 1, fp);
        }

      if (row % 16 == 0)
        gimp_progress_update ((gdouble) row / (gdouble) height);
    }

  g_object_unref (buffer);

  g_free (data);
  g_free (pixels);

  /* footer must be the last thing written to file */
  memset (footer, 0, 8); /* No extensions, no developer directory */
  memcpy (footer + 8, magic, sizeof (magic)); /* magic signature */
  fwrite (footer, sizeof (footer), 1, fp);

  fclose (fp);

  gimp_progress_update (1.0);

  return status;
}

static gboolean
save_dialog (GimpProcedure *procedure,
             GObject       *config)
{
  GtkWidget    *dialog;
  GtkWidget    *label;
  GtkWidget    *toggle;
  GtkListStore *store;
  GtkWidget    *combo;
  GtkWidget    *vbox;
  GtkWidget    *hbox;
  gboolean      run;

  dialog = gimp_procedure_dialog_new (procedure,
                                      GIMP_PROCEDURE_CONFIG (config),
                                      _("Export Image as TGA"));

  vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_container_set_border_width (GTK_CONTAINER (vbox), 12);
  gtk_box_pack_start (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (dialog))),
                      vbox, TRUE, TRUE, 0);
  gtk_widget_show (vbox);

  /*  rle  */
  toggle = gimp_prop_check_button_new (config, "rle",
                                       _("_RLE compression"));
  gtk_box_pack_start (GTK_BOX (vbox), toggle, FALSE, FALSE, 0);

  /*  origin  */
  hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);
  gtk_widget_show (hbox);

  label = gtk_label_new_with_mnemonic (_("Or_igin:"));
  gtk_box_pack_start (GTK_BOX (hbox), label, FALSE, FALSE, 0);
  gtk_widget_show (label);

  store = gimp_int_store_new (_("Bottom left"), ORIGIN_BOTTOM_LEFT,
                              _("Top left"),    ORIGIN_TOP_LEFT,
                              NULL);
  combo = gimp_prop_int_combo_box_new (config, "origin",
                                       GIMP_INT_STORE (store));
  gtk_box_pack_start (GTK_BOX (hbox), combo, TRUE, TRUE, 0);

  gtk_label_set_mnemonic_widget (GTK_LABEL (label), combo);

  gtk_widget_show (dialog);

  run = gimp_procedure_dialog_run (GIMP_PROCEDURE_DIALOG (dialog));

  gtk_widget_destroy (dialog);

  return run;
}
