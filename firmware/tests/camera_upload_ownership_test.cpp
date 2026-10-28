// AI-assisted by GPT-6 (OpenAI).
int main() {
    StackChanCamera camera;
    detector_hook = [&camera]() { camera.DetectorJoin(); };
    try {
        assert(camera.StreamJpegToBridge("http://test", "", {{"question", "test"}}) == "ok");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "upload failed after detector joined: %s; outstanding queues=%d buffers=%d\n", error.what(), queues, buffers);
        return 1;
    }
    assert(queues == 0 && buffers == 0 && !arbiter_held);
    assert(sent.find("JPEG") != std::string::npos && status_calls == 1);
    require_owned_frame = true;
    for (int fault = 0; fault < 5; ++fault) {
        fail_open = fault == 0; fail_encode = fault == 1; fail_write = fault == 2;
        http_status = fault == 3 ? 503 : 200; fail_capture_lock = fault == 4;
        bool failed = false;
        try { camera.StreamJpegToBridge("http://test", "", {}); }
        catch (const std::exception&) { failed = true; }
        assert(failed && queues == 0 && buffers == 0 && !arbiter_held);
    }
    fail_open = fail_encode = fail_write = fail_capture_lock = false; http_status = 200;
    assert(camera.StreamJpegToBridge("http://test", "", {}) == "ok");
    assert(queues == 0 && buffers == 0 && !arbiter_held);
}
