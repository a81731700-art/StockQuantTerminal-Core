"""StockQuant S10 RSI2 Pullback — FROZEN strategy.
Drop this file into vectorbt/custom_strategies/. Do not tune frozen defaults.
"""
import numpy as np
import pandas as pd

STRATEGY = {
    'type': 's10_rsi2_pullback_frozen',
    'name': 'S10 RSI2 Pullback (Frozen)',
    'category': 'StockQuant',
    'description': 'Close>SMA200 & RSI(2)<5; exit Close>SMA5 or max 5 bars',
    'parameters': [
        {'id': 'rsiPeriod', 'name': 'RSI Period', 'default': 2, 'min': 2, 'max': 2},
        {'id': 'oversold', 'name': 'RSI Entry', 'default': 5, 'min': 5, 'max': 5},
        {'id': 'trendSmaPeriod', 'name': 'Trend SMA', 'default': 200, 'min': 200, 'max': 200},
        {'id': 'exitSmaPeriod', 'name': 'Exit SMA', 'default': 5, 'min': 5, 'max': 5},
        {'id': 'maxHoldBars', 'name': 'Max Hold Bars', 'default': 5, 'min': 5, 'max': 5},
    ],
}

def _rsi(close, period):
    s = pd.Series(close, dtype=float)
    delta = s.diff()
    gain = delta.clip(lower=0).ewm(alpha=1/period, adjust=False).mean()
    loss = (-delta.clip(upper=0)).ewm(alpha=1/period, adjust=False).mean()
    rs = gain / loss.replace(0, np.nan)
    return (100 - 100/(1+rs)).fillna(100).to_numpy()

def build_signals(vbt, close_series, params, high_series=None, low_series=None, volume_series=None):
    close = close_series.to_numpy(dtype=float)
    idx = close_series.index
    rsi = _rsi(close, 2)
    sma200 = pd.Series(close).rolling(200).mean().to_numpy()
    sma5 = pd.Series(close).rolling(5).mean().to_numpy()
    raw_entry = (close > sma200) & (rsi < 5)
    entries = np.zeros(len(close), dtype=bool)
    exits = np.zeros(len(close), dtype=bool)
    active = False
    held = 0
    for i in range(len(close)):
        if not active:
            if raw_entry[i]:
                entries[i] = True
                active = True
                held = 0
        else:
            held += 1
            if close[i] > sma5[i] or held >= 5:
                exits[i] = True
                active = False
                held = 0
    return pd.Series(entries, index=idx), pd.Series(exits, index=idx)
