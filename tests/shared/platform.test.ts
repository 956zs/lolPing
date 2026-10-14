import { describe, expect, it } from 'vitest';
import { platformOf } from '../../src/shared/platform';

describe('platformOf', () => {
  it('names the three platforms lolPing runs on', () => {
    expect(platformOf('win32')).toBe('win');
    expect(platformOf('darwin')).toBe('mac');
    expect(platformOf('linux')).toBe('linux');
  });

  it('takes any other platform for Windows, as before Linux was added', () => {
    for (const p of ['freebsd', 'openbsd', 'aix', '', 'Linux']) expect(platformOf(p), p).toBe('win');
  });
});
