import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { autostartEntry, autostartUpdate, execArg, refreshAutostart, setAutostart } from '../../src/main/autostart';

describe('autostart', () => {
  describe('execArg', () => {
    it('leaves a plain argument as it is', () => {
      expect(execArg('/opt/lolPing/lolping')).toBe('/opt/lolPing/lolping');
      expect(execArg('--hidden')).toBe('--hidden');
    });

    it('quotes an argument with spaces or other reserved characters', () => {
      expect(execArg('/home/me/My Apps/lolPing.AppImage')).toBe('"/home/me/My Apps/lolPing.AppImage"');
      expect(execArg("/home/me/it's")).toBe('"/home/me/it\'s"');
      expect(execArg('')).toBe('""');
    });

    it('escapes quotes, backquotes, dollars and backslashes inside the quotes, and backslashes once more', () => {
      expect(execArg('a"b')).toBe('"a\\\\"b"');
      expect(execArg('a`b')).toBe('"a\\\\`b"');
      expect(execArg('$HOME')).toBe('"\\\\$HOME"');
      expect(execArg('a\\b')).toBe('"a\\\\\\\\b"');
    });

    it('doubles a percent sign, which would start a field code', () => {
      expect(execArg('/apps/100%/lolping')).toBe('/apps/100%%/lolping');
    });

    it('refuses an argument with a newline or another control character', () => {
      expect(execArg('a\nb')).toBeNull();
      expect(execArg('a\tb')).toBeNull();
      expect(execArg('a\u0000b')).toBeNull();
    });
  });

  describe('autostartEntry', () => {
    it('starts lolPing hidden in the tray', () => {
      expect(autostartEntry(['/home/me/Apps/lolPing.AppImage', '--hidden'])).toBe([
        '[Desktop Entry]',
        'Type=Application',
        'Name=lolPing',
        'Exec=/home/me/Apps/lolPing.AppImage --hidden',
        'Terminal=false',
        '',
      ].join('\n'));
    });

    it('quotes a path with spaces', () => {
      expect(autostartEntry(['/home/me/My Apps/lolPing.AppImage', '--hidden'])).toContain(
        'Exec="/home/me/My Apps/lolPing.AppImage" --hidden\n',
      );
    });

    it('is null without a command or with one a desktop file can\'t hold', () => {
      expect(autostartEntry([])).toBeNull();
      expect(autostartEntry(['/apps/lol\nPing', '--hidden'])).toBeNull();
    });
  });

  describe('autostartUpdate', () => {
    const argv = ['/home/me/Apps/lolPing-0.6.0.AppImage', '--hidden'];

    it('writes the entry when there is none', () => {
      expect(autostartUpdate(null, argv)).toBe(autostartEntry(argv));
    });

    it('writes nothing over an entry that already runs this copy', () => {
      expect(autostartUpdate(autostartEntry(argv), argv)).toBeNull();
    });

    it('rewrites an entry that runs another copy, such as the AppImage a new version replaced', () => {
      const old = autostartEntry(['/home/me/Apps/lolPing-0.5.2.AppImage', '--hidden']);
      expect(autostartUpdate(old, argv)).toBe(autostartEntry(argv));
      expect(autostartUpdate('', argv)).toBe(autostartEntry(argv));
    });

    it('writes nothing for a command a desktop file can\'t hold', () => {
      expect(autostartUpdate(null, ['/apps/lol\nPing'])).toBeNull();
      expect(autostartUpdate(null, [])).toBeNull();
    });
  });

  describe('refreshAutostart', () => {
    let dir: string;
    beforeEach(() => {
      dir = mkdtempSync(join(tmpdir(), 'lolping-autostart-'));
    });
    afterEach(() => rmSync(dir, { recursive: true, force: true }));

    it('points an old entry at this copy, and leaves an up-to-date one alone', async () => {
      const file = join(dir, 'autostart', 'lolping.desktop');
      await setAutostart(file, true, ['/apps/lolPing-0.5.2.AppImage', '--hidden']);
      expect(await refreshAutostart(file, ['/apps/lolPing-0.6.0.AppImage', '--hidden'])).toBe(true);
      expect(readFileSync(file, 'utf8')).toContain('Exec=/apps/lolPing-0.6.0.AppImage --hidden\n');
      const written = statSync(file).mtimeMs;
      expect(await refreshAutostart(file, ['/apps/lolPing-0.6.0.AppImage', '--hidden'])).toBe(false);
      expect(statSync(file).mtimeMs).toBe(written);
    });

    it('writes the entry when it is missing, making the folder', async () => {
      const file = join(dir, 'autostart', 'lolping.desktop');
      expect(await refreshAutostart(file, ['/apps/lolping', '--hidden'])).toBe(true);
      expect(readFileSync(file, 'utf8')).toContain('Exec=/apps/lolping --hidden\n');
    });

    it('fails when the entry can\'t be read for another reason than not being there', async () => {
      const file = join(dir, 'lolping.desktop');
      mkdirSync(file); // a folder where the file should be
      await expect(refreshAutostart(file, ['/apps/lolping', '--hidden'])).rejects.toThrow();
    });
  });

  describe('setAutostart', () => {
    let dir: string;
    beforeEach(() => {
      dir = mkdtempSync(join(tmpdir(), 'lolping-autostart-'));
    });
    afterEach(() => rmSync(dir, { recursive: true, force: true }));

    it('writes the entry, making the autostart folder, and removes it again', async () => {
      const file = join(dir, 'autostart', 'lolping.desktop');
      await setAutostart(file, true, ['/apps/lolping', '--hidden']);
      expect(readFileSync(file, 'utf8')).toContain('Exec=/apps/lolping --hidden\n');
      await setAutostart(file, false, ['/apps/lolping', '--hidden']);
      expect(existsSync(file)).toBe(false);
    });

    it('replaces an older entry, and removing a missing one is fine', async () => {
      const file = join(dir, 'lolping.desktop');
      writeFileSync(file, 'old');
      await setAutostart(file, true, ['/new/lolping', '--hidden']);
      expect(readFileSync(file, 'utf8')).toContain('Exec=/new/lolping --hidden\n');
      await setAutostart(file, false, []);
      await expect(setAutostart(file, false, [])).resolves.toBeUndefined();
    });

    it('fails without writing anything for a command a desktop file can\'t hold', async () => {
      const file = join(dir, 'lolping.desktop');
      await expect(setAutostart(file, true, ['/apps/lol\nPing'])).rejects.toThrow();
      expect(existsSync(file)).toBe(false);
    });
  });
});
