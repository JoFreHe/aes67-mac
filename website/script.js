const directionButtons = document.querySelectorAll('[data-direction]');
const signalStage = document.querySelector('.signal-stage');
const flowDescription = document.querySelector('#flow-description');

for (const button of directionButtons) {
  button.addEventListener('click', () => {
    const direction = button.dataset.direction;
    signalStage.dataset.flow = direction;
    flowDescription.textContent = direction === 'send'
      ? 'Play audio from your Mac to an AES67 receive stream.'
      : 'Record an AES67 transmit stream in a CoreAudio app.';

    for (const option of directionButtons) {
      const isActive = option === button;
      option.classList.toggle('active', isActive);
      option.setAttribute('aria-pressed', String(isActive));
    }
  });
}
