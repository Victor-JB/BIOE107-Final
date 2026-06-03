import React from 'react';
import { BrowserRouter, Routes, Route, Navigate } from 'react-router-dom';
import Nav from './components/Nav.jsx';
import Summary from './components/Summary.jsx';
import Timeline from './components/Timeline.jsx';
import History from './components/History.jsx';
import LiveView from './components/LiveView.jsx';
import Settings from './components/Settings.jsx';

export default function App() {
  return (
    <BrowserRouter>
      <div className="min-h-screen flex flex-col" style={{ background: '#0d1117' }}>
        <Nav />
        <main className="flex-1 container mx-auto px-4 py-6 max-w-4xl">
          <Routes>
            <Route path="/" element={<Navigate to="/summary" replace />} />
            <Route path="/summary" element={<Summary />} />
            <Route path="/timeline" element={<Timeline />} />
            <Route path="/history" element={<History />} />
            <Route path="/live" element={<LiveView />} />
            <Route path="/settings" element={<Settings />} />
          </Routes>
        </main>
      </div>
    </BrowserRouter>
  );
}
