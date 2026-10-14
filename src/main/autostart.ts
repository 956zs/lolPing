import { mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import { dirname } from 'node:path';

/** Characters that make an Exec argument need quotes (Desktop Entry spec, "The Exec key"). */
const RESERVED = /[\s"'\\><~|&;$*?#()`]/;

/** One Exec argument, quoted and escaped the way the Desktop Entry spec asks; null for one a desktop file can't hold. */
export function execArg(arg: string): string | null {
  if (/[\x00-\x1f\x7f]/.test(arg)) return null;
  const quoted = arg === '' || RESERVED.test(arg) ? `"${arg.replace(/["`$\\]/g, '\\$&')}"` : arg;
  // The value's own escapes are read before the quotes, so every backslash is written twice; a lone % starts a field code.
  return quoted.replace(/\\/g, '\\\\').replace(/%/g, '%%');
}

/** The XDG autostart entry that runs `argv` (the executable first) at login; null when it can't be written as one. */
export function autostartEntry(argv: readonly string[]): string | null {
  if (argv.length === 0) return null;
  const args: string[] = [];
  for (const a of argv) {
    const arg = execArg(a);
    if (arg === null) return null;
    args.push(arg);
  }
  return ['[Desktop Entry]', 'Type=Application', 'Name=lolPing', `Exec=${args.join(' ')}`, 'Terminal=false', ''].join('\n');
}

/** Linux "open at login": writes the autostart entry for `argv` to `file`, or removes it. */
export async function setAutostart(file: string, on: boolean, argv: readonly string[]): Promise<void> {
  if (!on) {
    await rm(file, { force: true });
    return;
  }
  const entry = autostartEntry(argv);
  if (entry === null) throw new Error(`can't write ${argv[0] ?? 'nothing'} as a desktop entry's Exec`);
  await mkdir(dirname(file), { recursive: true });
  await writeFile(file, entry, 'utf8');
}

/** What to write over an autostart file holding `current` (null: none) so it runs `argv`; null when it already does. */
export function autostartUpdate(current: string | null, argv: readonly string[]): string | null {
  const entry = autostartEntry(argv);
  return entry === null || entry === current ? null : entry;
}

/**
 * Linux, at startup with "open at login" on: points the entry at this copy of lolPing, since a newer AppImage replacing
 * the old file has another path. Writes only when the entry differs. Resolves with whether it wrote.
 */
export async function refreshAutostart(file: string, argv: readonly string[]): Promise<boolean> {
  let current: string | null = null;
  try {
    current = await readFile(file, 'utf8');
  } catch (err) {
    if ((err as NodeJS.ErrnoException).code !== 'ENOENT') throw err;
  }
  const entry = autostartUpdate(current, argv);
  if (entry === null) return false;
  await mkdir(dirname(file), { recursive: true });
  await writeFile(file, entry, 'utf8');
  return true;
}
