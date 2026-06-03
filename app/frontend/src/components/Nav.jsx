import React from 'react';
import { NavLink } from 'react-router-dom';

const links = [
  { to: '/summary', label: 'Summary' },
  { to: '/timeline', label: 'Timeline' },
  { to: '/history', label: 'History' },
  { to: '/live', label: 'Live' },
  { to: '/settings', label: 'Settings' },
];

export default function Nav() {
  return (
    <nav style={{ background: '#161b22', borderBottom: '1px solid #21262d' }}>
      <div className="container mx-auto px-4 max-w-4xl flex items-center justify-between h-14">
        <span className="font-semibold text-white text-sm tracking-wide">Sleep Monitor</span>
        <div className="flex gap-1">
          {links.map(({ to, label }) => (
            <NavLink
              key={to}
              to={to}
              className={({ isActive }) =>
                `px-3 py-1.5 rounded text-sm transition-colors ${
                  isActive
                    ? 'bg-blue-600 text-white'
                    : 'text-gray-400 hover:text-white hover:bg-white/5'
                }`
              }
            >
              {label}
            </NavLink>
          ))}
        </div>
      </div>
    </nav>
  );
}
