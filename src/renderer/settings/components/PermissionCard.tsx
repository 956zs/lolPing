import { Button } from '@fluentui/react-components';
import { ShieldKeyhole24Regular } from '@fluentui/react-icons';
import type { PermStep } from '../../../shared/i18n';
import type { AccessProblem } from '../../../shared/protocol';
import { api } from '../api';
import { useText } from '../text';

/**
 * macOS only: shown while lolPing has no Accessibility access. `stale`: macOS lists lolPing as allowed but the
 * helper is still refused, which is what an entry left over from an earlier (differently signed) build looks like.
 */
export function PermissionCard({ stale }: { stale: boolean }) {
  const t = useText();
  return (
    <div className="card permission">
      <span className="icon"><ShieldKeyhole24Regular /></span>
      <div className="lbl">
        <b>{stale ? t.permStaleTitle : t.permTitle}</b>
        <ol className="desc">
          {(stale ? t.permStaleSteps : t.permSteps).map((step) => <li key={step}>{step}</li>)}
        </ol>
        <div className="actions">
          <Button appearance="primary" onClick={() => void api.openAccessibility()}>{t.permOpen}</Button>
          {stale ? <Button onClick={() => void api.retryHelper()}>{t.permRetry}</Button> : null}
        </div>
      </div>
    </div>
  );
}

/** Linux only: shown while the helper can't open the input devices, or runs outside a Hyprland session. */
export function LinuxPermissionCard({ problem }: { problem: AccessProblem }) {
  const t = useText();
  const steps: PermStep[] = problem === 'hyprland' ? t.permHyprlandSteps.map((text) => ({ text })) : t.permLinuxSteps;
  return (
    <div className="card permission">
      <span className="icon"><ShieldKeyhole24Regular /></span>
      <div className="lbl">
        <b>{problem === 'hyprland' ? t.permHyprlandTitle : t.permLinuxTitle}</b>
        <ol className="desc">
          {steps.map((step) => (
            <li key={step.text}>
              {step.text}
              {step.command ? <pre className="command">{step.command}</pre> : null}
            </li>
          ))}
        </ol>
        <div className="actions">
          <Button appearance="primary" onClick={() => void api.retryHelper()}>{t.permRetry}</Button>
        </div>
      </div>
    </div>
  );
}
