import React, { useEffect, useState } from 'react';

export default function Settings() {
  const [settings, setSettings] = useState(null);
  const [saving, setSaving] = useState(false);
  const [saved, setSaved] = useState(false);

  useEffect(() => {
    fetch('/api/settings').then(r => r.json()).then(setSettings).catch(console.error);
  }, []);

  async function save() {
    setSaving(true);
    await fetch('/api/settings', { method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(settings) });
    setSaving(false);
    setSaved(true);
    setTimeout(() => setSaved(false), 2000);
  }

  async function generateMockNight(scenario) {
    await fetch('/api/generate', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ scenario }) });
    alert(`Generated a new mock night (${scenario} scenario). Refresh the Summary page.`);
  }

  if (!settings) return <div className="text-gray-500 text-sm py-12 text-center">Loading settings…</div>;

  return (
    <div className="space-y-6 max-w-lg">
      <h1 className="text-lg font-semibold text-white">Settings</h1>

      {/* Haptic settings */}
      <section className="rounded-lg p-4 space-y-4" style={{ background: '#161b22', border: '1px solid #21262d' }}>
        <h2 className="text-sm font-semibold text-white">Haptic feedback</h2>
        <div className="flex items-center justify-between">
          <div>
            <div className="text-sm text-gray-300">Vibration alerts</div>
            <div className="text-xs text-gray-500">Nudge when breathing disruption detected</div>
          </div>
          <button
            onClick={() => setSettings(s => ({ ...s, haptic_enabled: s.haptic_enabled === 'true' ? 'false' : 'true' }))}
            className="relative inline-flex h-6 w-11 items-center rounded-full transition-colors"
            style={{ background: settings.haptic_enabled === 'true' ? '#1f6feb' : '#21262d' }}
          >
            <span
              className="inline-block h-4 w-4 transform rounded-full bg-white transition-transform"
              style={{ transform: settings.haptic_enabled === 'true' ? 'translateX(24px)' : 'translateX(4px)' }}
            />
          </button>
        </div>
        {settings.haptic_enabled === 'true' && (
          <div>
            <div className="flex justify-between text-xs text-gray-400 mb-1">
              <span>Intensity</span>
              <span>{['Off', 'Gentle', 'Medium', 'Strong'][parseInt(settings.haptic_intensity)] || 'Medium'}</span>
            </div>
            <input
              type="range" min="1" max="3" step="1"
              value={settings.haptic_intensity}
              onChange={e => setSettings(s => ({ ...s, haptic_intensity: e.target.value }))}
              className="w-full accent-blue-500"
            />
          </div>
        )}
      </section>

      <button
        onClick={save}
        disabled={saving}
        className="w-full py-2.5 rounded-lg text-sm font-medium text-white transition-colors disabled:opacity-50"
        style={{ background: '#1f6feb' }}
      >
        {saved ? '✓ Saved' : saving ? 'Saving…' : 'Save settings'}
      </button>

      {/* Dev tools */}
      <section className="rounded-lg p-4 space-y-3" style={{ background: '#161b22', border: '1px solid #21262d' }}>
        <h2 className="text-sm font-semibold text-white">Developer tools</h2>
        <p className="text-xs text-gray-500">Generate mock overnight data for testing</p>
        <div className="flex gap-2 flex-wrap">
          {['low', 'moderate', 'high'].map(s => (
            <button
              key={s}
              onClick={() => generateMockNight(s)}
              className="px-3 py-1.5 rounded text-xs font-medium capitalize transition-colors hover:opacity-80"
              style={{ background: '#21262d', color: '#e6edf3', border: '1px solid #30363d' }}
            >
              Generate {s} risk night
            </button>
          ))}
        </div>
      </section>

      {/* About */}
      <section className="rounded-lg p-4 space-y-3" style={{ background: '#161b22', border: '1px solid #21262d' }}>
        <h2 className="text-sm font-semibold text-white">About this device</h2>
        <div className="space-y-2 text-xs text-gray-400 leading-relaxed">
          <p>This wearable sleep monitor estimates patterns associated with sleep-disordered breathing using forehead SpO₂, snore audio, body position, and nasal airflow sensors.</p>
          <p><strong className="text-gray-300">What it measures:</strong> Blood oxygen saturation (SpO₂), heart rate, snore intensity, body position, and movement during sleep.</p>
          <p><strong className="text-gray-300">What it cannot do:</strong> This device cannot diagnose sleep apnea or any other medical condition. It is a screening tool only. Clinical diagnosis requires a polysomnogram (sleep study) interpreted by a licensed sleep physician.</p>
          <p><strong className="text-gray-300">Sensor limitations:</strong> Forehead reflectance pulse oximetry is affected by motion, ambient light, and skin contact quality. Results should be interpreted as estimates with significant uncertainty.</p>
          <p><strong className="text-gray-300">If you're concerned:</strong> Ask your primary care provider about a referral for a sleep study (polysomnogram). Home sleep testing is also available through many providers.</p>
        </div>
        <div className="pt-2 text-xs text-gray-600" style={{ borderTop: '1px solid #21262d' }}>
          SCU Bioe107 Class Project · For educational and demonstration purposes only
        </div>
      </section>
    </div>
  );
}
