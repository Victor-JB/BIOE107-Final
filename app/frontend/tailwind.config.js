/** @type {import('tailwindcss').Config} */
export default {
  content: ['./index.html', './src/**/*.{js,jsx}'],
  theme: {
    extend: {
      colors: {
        surface: '#1a1a2e',
        card: '#16213e',
        accent: '#0f3460',
        'risk-low': '#22c55e',
        'risk-moderate': '#f59e0b',
        'risk-high': '#ef4444',
      },
    },
  },
  plugins: [],
};
