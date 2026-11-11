import unittest

from verify_native_collectible import grade, reject_controls, rotate


def fixture():
    base = 0x140000000
    state = dict(kind="state", hero=1, item=2, world=3, basic=2, special=49, selected=2,
                 flags=0, root=[0, 0, 0], steam_disabled=True, native_forward=[1, 0, 0])
    final = dict(state, world=899, basic=0, special=0, selected=0, flags=1)
    collected = dict(kind="collect", hero=1, item=2, tag=42, world=110, flags_before=0, flags_after=1,
                     collected_before=1, collected_after=2, unread_before=0, unread_after=1,
                     had_tag_before=False, has_tag_after=True, unread_had_tag_before=False,
                     unread_has_tag_after=True, hud_tag=42)
    use = dict(kind="pickup", action=1, result=1, hero=1, selected_pickup=2, controller_aim=True,
               special=49, view_restored=True, thread=5, input_thread=5, clock_us=1200,
               origin=[0, 30, -45], direction=[-1, 0, 0], body_origin=[0, 0, 0],
               native_horizontal_range=[15, 100],
               pickup_candidates=[dict(actor=2, vtable=base+0x1AAE460, origin=[-80, 30, -45])])
    pad = dict(clock_us=1150, xr_time=4*11111111, buttons=16384, focused=True,
               right_aim_tracked=True, use_ready=True)
    return dict(observer=[
        dict(kind="observer", module_base=base, writes_native_state=False, original_calls_per_hook=1),
        state, dict(kind="notify", hero=1, world=5, selected_before=2, selected_after=2,
                    special_before=49, special_after=49), collected, final],
        uses=[use], pads=[pad, dict(pad, clock_us=1300, xr_time=6*11111111)],
        fixtures=[dict(world=i, route_stage=i, trigger=int(i == 3), aim=[0, 1, 0, 0]) for i in range(4)],
        frames=[dict(frame=4, gameplay_frame=3), dict(frame=6, gameplay_frame=105)],
        bodies=[dict(hero=1, clock_us=1000, root=[0, 0, 0], targets=dict(
            origin=[-40, 0, 0], right=[0, 1, 0], up=[0, 0, 1], backward=[-1, 0, 0],
            units_per_meter=100, hands=[{}, dict(controller_position=[.3, -.45, -.4])]))])


class NativeCollectibleTests(unittest.TestCase):
    def test_genuine_collection_contract(self):
        result = grade(fixture())
        self.assertEqual(result["native_document_collections"], 1)
        self.assertEqual(result["continuation_input_samples"], 1)

    def test_corruption_controls(self):
        controls = reject_controls(fixture())
        self.assertEqual(len(controls), 14)
        continuation = next(r for r in controls if r["control"] == "missing-continuation")
        self.assertIn("continuation was blocked", continuation["rejected"])

    def test_authored_range(self):
        data = fixture()
        data["uses"][0]["body_origin"] = [1000, 0, 0]
        with self.assertRaisesRegex(ValueError, "body range"):
            grade(data)

    def test_controller_pose_cannot_be_fabricated(self):
        data = fixture()
        data["uses"][0]["origin"][1] += 1
        with self.assertRaisesRegex(ValueError, "reconstruction"):
            grade(data)

    def test_actor_discontinuity(self):
        data = fixture()
        data["observer"][-1]["root"] = [1000, 0, 0]
        with self.assertRaisesRegex(ValueError, "discontinuity"):
            grade(data)

    def test_invalid_controller_rotation(self):
        for q in ([0, 0, 0, 0], [0, 0, 0, float("nan")]):
            with self.assertRaisesRegex(ValueError, "quaternion"):
                rotate(q, [0, 0, -1])
        self.assertEqual(rotate([0, 0, 0, 1], [0, 0, -1]), [0, 0, -1])


if __name__ == "__main__":
    unittest.main()
