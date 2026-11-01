package net.abitante.indigo;

import android.content.Intent;
import android.database.Cursor;
import android.os.Bundle;
import android.net.Uri;
import android.provider.OpenableColumns;
import android.view.HapticFeedbackConstants;
import android.view.View;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;

import org.libsdl.app.SDLActivity;

// Atividade do Índigo: a do SDL mais dois serviços chamados pelo C++ (src/app/platform.cpp).
public class IndigoActivity extends SDLActivity {
    // ---- jogos pedidos por frontends (ES-DE, Daijisho, Beacon, Pegasus, LaunchBox) ou pelo "Abrir com"
    // Aceita o endereço do jogo em getData() (content:// ou file://) ou num extra com o caminho (ROM, rom,
    // path, file, filePath, LaunchFile), como os frontends costumam mandar para emuladores.
    private static final String[] ROM_EXTRAS = { "ROM", "rom", "path", "file", "filePath", "LaunchFile" };
    private String pendingRom;

    static String romFromIntent(Intent it) {
        if (it == null) return null;
        Uri d = it.getData();
        if (d != null) {
            if ("file".equals(d.getScheme()) && d.getPath() != null) return d.getPath();
            return d.toString();
        }
        Bundle ex = it.getExtras();
        if (ex != null) {
            for (String k : ROM_EXTRAS) {
                Object v = ex.get(k);
                if (v instanceof String && !((String) v).isEmpty()) return (String) v;
                if (v instanceof Uri) return v.toString();
            }
        }
        return null;
    }

    private String launchRom;

    // o pedido é tirado da intent antes do SDL olhar (ele abriria getData().getPath(), que não serve para
    // endereços content://)
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        launchRom = romFromIntent(getIntent());
        if (getIntent() != null) getIntent().setData(null);
        super.onCreate(savedInstanceState);
    }

    // primeiro jogo: vai como argumento do main() do C++ (--frontend = tela cheia, sair ao fechar o jogo)
    @Override
    protected String[] getArguments() {
        if (launchRom == null) return new String[0];
        String rom = launchRom;
        launchRom = null;
        return new String[] { "--frontend", rom };
    }

    // app já aberto (singleInstance): o pedido novo fica guardado até o C++ buscar (takePendingRom)
    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        String rom = romFromIntent(intent);
        if (rom != null) synchronized (this) { pendingRom = rom; }
    }

    public synchronized String takePendingRom() {
        String r = pendingRom;
        pendingRom = null;
        return r == null ? "" : r;
    }

    // nome do arquivo escolhido no seletor do Android (o caminho vem como content://...)
    public String displayName(String uri) {
        try (Cursor c = getContentResolver().query(Uri.parse(uri),
                new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                String n = c.getString(0);
                if (n != null && !n.isEmpty()) return n;
            }
        } catch (Exception e) {
            // segue para o nome tirado do próprio endereço
        }
        String s = Uri.decode(uri);
        int i = Math.max(s.lastIndexOf('/'), s.lastIndexOf(':'));
        return i >= 0 ? s.substring(i + 1) : s;
    }

    // vibração curta ao apertar um botão da tela
    public void haptic() {
        runOnUiThread(() -> {
            View v = getWindow().getDecorView();
            v.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
        });
    }

    // requisição HTTP para a nuvem dos saves (chamada fora da linha da interface).
    // Resposta: status (4 bytes, big-endian), tipo do conteúdo + "\n", corpo. Status 0 = erro (corpo = mensagem).
    public byte[] httpRequest(String method, String url, String pin, byte[] body, int timeoutMs) {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL(url).openConnection();
            c.setRequestMethod(method);
            c.setConnectTimeout(timeoutMs);
            c.setReadTimeout(timeoutMs);
            c.setUseCaches(false);
            c.setRequestProperty("Cache-Control", "no-store");
            if (pin != null && !pin.isEmpty()) c.setRequestProperty("x-indigo-pin", pin);
            if (body != null) {
                c.setDoOutput(true);
                c.setRequestProperty("Content-Type", "application/octet-stream");
                c.setFixedLengthStreamingMode(body.length);
                try (OutputStream os = c.getOutputStream()) { os.write(body); }
            }
            int status = c.getResponseCode();
            InputStream in = status >= 400 ? c.getErrorStream() : c.getInputStream();
            String type = c.getContentType();
            out.write(new byte[] { (byte) (status >> 24), (byte) (status >> 16), (byte) (status >> 8), (byte) status });
            out.write(((type == null ? "" : type) + "\n").getBytes(StandardCharsets.UTF_8));
            if (in != null) {
                byte[] buf = new byte[16384];
                int n;
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
                in.close();
            }
            return out.toByteArray();
        } catch (Exception e) {
            ByteArrayOutputStream err = new ByteArrayOutputStream();
            err.write(0); err.write(0); err.write(0); err.write(0);
            byte[] msg = ("\nSem conexão com a nuvem (" + e.getClass().getSimpleName() + ").").getBytes(StandardCharsets.UTF_8);
            err.write(msg, 0, msg.length);
            return err.toByteArray();
        } finally {
            if (c != null) c.disconnect();
        }
    }
}
