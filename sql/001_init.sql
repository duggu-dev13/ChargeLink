-- Step 5: quick test table. The real schema comes in Phase 4.
CREATE TABLE IF NOT EXISTS charge_points (
    id          TEXT PRIMARY KEY,
    vendor      TEXT,
    model       TEXT,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

INSERT INTO charge_points (id, vendor, model)
VALUES ('CP001', 'SCharge', 'A1-7.4kW')
ON CONFLICT (id) DO NOTHING;