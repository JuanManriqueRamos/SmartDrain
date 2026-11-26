/**
 * ============================================================================
 * config.js - Lógica interactiva y validación pedagógica de config.html
 * ============================================================================
 */

(function () {
  const minInput = document.getElementById('min');
  const maxInput = document.getElementById('max');
  const safeBar = document.getElementById('previewSafeBar');
  const safeLabel = document.getElementById('previewSafeLabel');
  const form = document.getElementById('configForm');
  const linkBack = document.getElementById('linkBackDashboard');
  const localFeedback = document.getElementById('localFeedback');

  if (!minInput || !maxInput) return;

  // Si se abre como archivo local sin servidor Flask, limpiar posibles llaves Jinja en los inputs
  if (minInput.value.includes('{{') || minInput.value === '') minInput.value = 18;
  if (maxInput.value.includes('{{') || maxInput.value === '') maxInput.value = 95;

  if (window.location.protocol === 'file:' && linkBack) {
    linkBack.setAttribute('href', 'index.html');
  }

  function updateVisualizer() {
    const minVal = Math.max(5, Math.min(399, parseFloat(minInput.value) || 5));
    const maxVal = Math.max(6, Math.min(400, parseFloat(maxInput.value) || 400));

    const leftPct = (minVal / 400) * 100;
    const widthPct = Math.max(1, ((maxVal - minVal) / 400) * 100);

    if (safeBar) {
      safeBar.style.left = `${leftPct}%`;
      safeBar.style.width = `${widthPct}%`;
      safeBar.style.background = (minVal < maxVal) ? 'var(--status-ok)' : 'var(--status-err)';
    }

    if (safeLabel) {
      if (minVal < maxVal) {
        safeLabel.textContent = `Zona Segura: ${minVal} cm – ${maxVal} cm`;
        safeLabel.style.color = 'var(--status-ok)';
      } else {
        safeLabel.textContent = `Rango inválido: Mínimo debe ser menor que Máximo`;
        safeLabel.style.color = 'var(--status-err)';
      }
    }
  }

  minInput.addEventListener('input', updateVisualizer);
  maxInput.addEventListener('input', updateVisualizer);

  form.addEventListener('submit', function (e) {
    const minVal = parseFloat(minInput.value);
    const maxVal = parseFloat(maxInput.value);

    if (minVal >= maxVal) {
      e.preventDefault();
      alert('Error de validación: La distancia mínima debe ser menor que la distancia máxima.');
      return;
    }

    // Guardar también en localStorage para que funcione incluso si se prueba abriendo los HTML localmente
    localStorage.setItem('edtech_iot_config', JSON.stringify({
      min_distance_cm: minVal,
      max_distance_cm: maxVal,
      alert_sensor_error: document.getElementById('chkSensorError').checked,
      alert_recovery: document.getElementById('chkRecovery').checked
    }));

    if (window.location.protocol === 'file:') {
      e.preventDefault();
      localFeedback.style.display = 'block';
      localFeedback.textContent = '✅ Configuración guardada localmente. Regresa al dashboard para ver los nuevos umbrales.';
    }
  });

  updateVisualizer();
})();
