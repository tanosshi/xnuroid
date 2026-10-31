package com.xnuroid;

import android.app.NativeActivity;
import android.content.ClipData;
import android.content.Intent;
import android.database.Cursor;
import android.media.AudioAttributes;
import android.media.MediaMetadataRetriever;
import android.media.MediaPlayer;
import android.net.Uri;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.MediaStore;
import android.view.Gravity;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.ViewGroup;
import android.widget.FrameLayout;

import java.io.File;
import java.util.ArrayList;
import java.util.concurrent.ConcurrentHashMap;

// NativeActivity plus what native code cannot do on its own: receive an activity result, and show views. The guest's
// document picker (UIDocumentPickerViewController) opens the system picker through pickDocuments, and the chosen
// documents come back to native code (android_app.cpp) as file paths. AVPlayer and AVAudioPlayer play through the
// media* methods: MediaPlayer decodes, and a video shows in a SurfaceView laid over the game.
public class HostActivity extends NativeActivity {
    private static final int PICK = 0x5843;
    private boolean pickingFolder;

    private static native void onDocumentsPicked(String[] paths);

    public void pickDocuments(final boolean folder, final boolean multiple) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                pickingFolder = folder;
                Intent intent;
                if (folder) {
                    intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
                } else {
                    intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                    intent.addCategory(Intent.CATEGORY_OPENABLE);
                    intent.setType("*/*");
                    intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, multiple);
                }
                try {
                    startActivityForResult(intent, PICK);
                } catch (Exception e) {
                    onDocumentsPicked(new String[0]);
                }
            }
        });
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        if (request != PICK) {
            super.onActivityResult(request, result, data);
            return;
        }
        ArrayList<String> paths = new ArrayList<>();
        if (result == RESULT_OK && data != null) {
            ClipData clip = data.getClipData();
            if (clip != null) {
                for (int i = 0; i < clip.getItemCount(); ++i) add(paths, clip.getItemAt(i).getUri());
            } else if (data.getData() != null) {
                add(paths, data.getData());
            }
        }
        onDocumentsPicked(paths.toArray(new String[0]));
    }

    private void add(ArrayList<String> out, Uri uri) {
        String path = pathOf(uri);
        if (path != null) out.add(path);
    }

    // A file path for a picked document. Documents on device storage map to their real path (the app holds
    // all-files access); anything else is opened here and handed over as /proc/self/fd/N.
    private String pathOf(Uri uri) {
        if ("file".equals(uri.getScheme())) return uri.getPath();
        try {
            String id = pickingFolder ? DocumentsContract.getTreeDocumentId(uri) : DocumentsContract.getDocumentId(uri);
            String authority = uri.getAuthority();
            if ("com.android.externalstorage.documents".equals(authority)) {
                int colon = id.indexOf(':');
                String volume = id.substring(0, colon), rest = id.substring(colon + 1);
                String root = "primary".equalsIgnoreCase(volume) ? Environment.getExternalStorageDirectory().getPath() : "/storage/" + volume;
                return rest.isEmpty() ? root : root + "/" + rest;
            }
            if ("com.android.providers.downloads.documents".equals(authority) && id.startsWith("raw:")) return id.substring(4);
            // Downloads ("msf:N") and media ("video:N", "document:N") documents are MediaStore rows with a path
            boolean downloads = "com.android.providers.downloads.documents".equals(authority) && id.startsWith("msf:");
            if (downloads || "com.android.providers.media.documents".equals(authority)) {
                String path = mediaPath(id.substring(id.indexOf(':') + 1));
                if (path != null) return path;
            }
        } catch (Exception e) {
            // not a document URI we can map; fall through
        }
        if (pickingFolder) return null;
        try {
            // last resort: an open descriptor, readable through /proc
            ParcelFileDescriptor fd = getContentResolver().openFileDescriptor(uri, "r");
            return fd == null ? null : "/proc/self/fd/" + fd.detachFd();
        } catch (Exception e) {
            return null;
        }
    }

    // ---- media ----
    private static native void onMediaEvent(int id, int event);   // 1 played to the end, 2 failed

    private final ConcurrentHashMap<Integer, Media> media = new ConcurrentHashMap<>();

    private final class Media implements SurfaceHolder.Callback {
        final int id;
        final boolean video;
        final MediaPlayer player = new MediaPlayer();
        SurfaceView view;
        volatile boolean prepared, released;
        boolean playing, loop;
        float volume = 1;
        int seekMs = -1, left, top, width, height, videoW, videoH;

        Media(int id, boolean video) { this.id = id; this.video = video; }

        @Override public void surfaceCreated(SurfaceHolder h) { if (!released) player.setDisplay(h); }
        @Override public void surfaceChanged(SurfaceHolder h, int format, int w, int ht) {}
        @Override public void surfaceDestroyed(SurfaceHolder h) { if (!released) player.setDisplay(null); }

        // the video keeps its aspect ratio inside the frame it was given (AVLayerVideoGravityResizeAspect)
        FrameLayout.LayoutParams layout() {
            int w = width, h = height;
            if (videoW > 0 && videoH > 0 && w > 0 && h > 0) {
                if ((long) w * videoH > (long) h * videoW) w = (int) ((long) h * videoW / videoH);
                else h = (int) ((long) w * videoH / videoW);
            }
            FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(Math.max(w, 1), Math.max(h, 1), Gravity.TOP | Gravity.LEFT);
            lp.leftMargin = left + (width - w) / 2;
            lp.topMargin = top + (height - h) / 2;
            return lp;
        }
    }

    public void mediaOpen(final int id, final String path, final boolean video) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                final Media m = new Media(id, video);
                media.put(id, m);
                try {
                    m.player.setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME)
                            .setContentType(video ? AudioAttributes.CONTENT_TYPE_MOVIE : AudioAttributes.CONTENT_TYPE_MUSIC).build());
                    m.player.setDataSource(path);
                    m.player.setOnPreparedListener(new MediaPlayer.OnPreparedListener() {
                        @Override
                        public void onPrepared(MediaPlayer mp) {
                            m.prepared = true;
                            mp.setVolume(m.volume, m.volume);
                            mp.setLooping(m.loop);
                            if (m.seekMs >= 0) mp.seekTo(m.seekMs);
                            if (m.playing) mp.start();
                        }
                    });
                    m.player.setOnCompletionListener(new MediaPlayer.OnCompletionListener() {
                        @Override
                        public void onCompletion(MediaPlayer mp) {
                            m.playing = false;
                            onMediaEvent(id, 1);
                        }
                    });
                    m.player.setOnErrorListener(new MediaPlayer.OnErrorListener() {
                        @Override
                        public boolean onError(MediaPlayer mp, int what, int extra) {
                            m.playing = false;
                            onMediaEvent(id, 2);
                            return true;
                        }
                    });
                    m.player.setOnVideoSizeChangedListener(new MediaPlayer.OnVideoSizeChangedListener() {
                        @Override
                        public void onVideoSizeChanged(MediaPlayer mp, int w, int h) {
                            m.videoW = w;
                            m.videoH = h;
                            if (m.view != null) m.view.setLayoutParams(m.layout());
                        }
                    });
                    m.player.prepareAsync();
                } catch (Exception e) {
                    onMediaEvent(id, 2);
                }
            }
        });
    }

    // starts (or resumes) playback; a video is shown in the given window rectangle
    public void mediaPlay(final int id, final int left, final int top, final int width, final int height) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                Media m = media.get(id);
                if (m == null) return;
                m.playing = true;
                if (m.video) {
                    m.left = left;
                    m.top = top;
                    m.width = width;
                    m.height = height;
                    if (m.view == null) {
                        m.view = new SurfaceView(HostActivity.this);
                        m.view.setZOrderMediaOverlay(true);   // above the game's surface
                        m.view.getHolder().addCallback(m);
                        addContentView(m.view, m.layout());
                    } else {
                        m.view.setLayoutParams(m.layout());
                    }
                }
                if (m.prepared && !m.player.isPlaying()) m.player.start();
            }
        });
    }

    public void mediaPause(final int id) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                Media m = media.get(id);
                if (m == null) return;
                m.playing = false;
                if (m.prepared && m.player.isPlaying()) m.player.pause();
            }
        });
    }

    public void mediaClose(final int id) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                Media m = media.remove(id);
                if (m == null) return;
                m.released = true;
                m.player.release();
                if (m.view != null && m.view.getParent() != null) ((ViewGroup) m.view.getParent()).removeView(m.view);
            }
        });
    }

    public void mediaSeek(final int id, final int ms) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                Media m = media.get(id);
                if (m == null) return;
                if (m.prepared) m.player.seekTo(ms);
                else m.seekMs = ms;
            }
        });
    }

    public void mediaSetVolume(final int id, final float volume) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                Media m = media.get(id);
                if (m == null) return;
                m.volume = volume;
                if (m.prepared) m.player.setVolume(volume, volume);
            }
        });
    }

    public void mediaSetLoop(final int id, final boolean loop) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                Media m = media.get(id);
                if (m == null) return;
                m.loop = loop;
                if (m.prepared) m.player.setLooping(loop);
            }
        });
    }

    public int mediaPosition(int id) {
        Media m = media.get(id);
        try {
            return m != null && m.prepared && !m.released ? m.player.getCurrentPosition() : 0;
        } catch (Exception e) {
            return 0;
        }
    }

    public static int mediaDuration(String path) {
        MediaMetadataRetriever r = new MediaMetadataRetriever();
        try {
            r.setDataSource(path);
            String d = r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_DURATION);
            return d == null ? 0 : Integer.parseInt(d);
        } catch (Exception e) {
            return 0;
        } finally {
            try { r.release(); } catch (Exception e) { /* already released */ }
        }
    }

    // playback follows the activity: paused in the background, as on iOS
    @Override
    protected void onPause() {
        super.onPause();
        for (Media m : media.values()) if (m.prepared && m.player.isPlaying()) m.player.pause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        for (Media m : media.values()) if (m.prepared && m.playing && !m.player.isPlaying()) m.player.start();
    }

    private String mediaPath(String id) {
        String[] column = {MediaStore.MediaColumns.DATA};
        try (Cursor c = getContentResolver().query(MediaStore.Files.getContentUri("external"), column, "_id=?", new String[]{id}, null)) {
            if (c != null && c.moveToFirst()) {
                String path = c.getString(0);
                if (path != null && new File(path).canRead()) return path;
            }
        } catch (Exception e) {
            // no MediaStore row for it
        }
        return null;
    }
}
