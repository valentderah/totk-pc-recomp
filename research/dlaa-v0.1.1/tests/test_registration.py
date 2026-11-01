import unittest
from analyze import registration_status

class RegistrationTests(unittest.TestCase):
    def test_large_error_and_low_support_reject_history(self):
        self.assertEqual(registration_status(20,30,0.7,0.7,False),'rejected')
        self.assertEqual(registration_status(0.1,0.5,0.001,0.9,False),'rejected')
        self.assertEqual(registration_status(0.1,0.5,0.6,0.9,True),'rejected')
        self.assertEqual(registration_status(0.1,0.5,0.6,0.9,False),'accepted_for_diagnostic')
