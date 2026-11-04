import analyze


def test_visits_and_accept():
    ev = [
        {"ev": "start", "t": 0.0},
        {"ev": "cmd", "t": 0.01, "t_start": 0.0},
        {"ev": "result", "t": 0.03, "uid": "a0102030", "auth": "fail"},
        {"ev": "result", "t": 0.06, "uid": "a0102030", "auth": "fail"},
        {"ev": "result", "t": 0.09, "uid": "a1112131", "auth": "fail"},
        {"ev": "result", "t": 0.12, "uid": "a2122232", "auth": "ok"},
        {"ev": "accept", "t": 0.15},
        {"ev": "end", "t": 1.7},
    ]
    r = analyze.analyze(ev)
    assert r["accepted"] and r["time_to_accept_ms"] == 150.0
    assert [v["slot"] for v in r["visits"]] == [0, 1, 2]
    assert r["switches"] == 2 and r["visits"][0]["n"] == 2


def test_chameleon_summary():
    log = [
        {"event": "AUTH_FAIL", "t": 1.000, "slot": 0, "arg": 0},
        {"event": "SWITCH", "t": 1.004, "slot": 1, "arg": 0},
        {"event": "SWITCH", "t": 2.0, "slot": 2, "arg": 1},
    ]
    s = analyze.chameleon_summary(log)
    assert s["switch_after_auth_fail_ms"] == [4.0] and s["switch_count"] == 2


if __name__ == "__main__":
    test_visits_and_accept()
    test_chameleon_summary()
    print("ok")
