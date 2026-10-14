import { mkdir, rm, writeFile } from 'node:fs/promises';
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
