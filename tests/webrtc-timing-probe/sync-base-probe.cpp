// Actual upstream synchronization with deterministic receiver-floor feedback.
// This models receiver delays, not NetEQ, codecs, network or physical playout.
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "video/stream_synchronization.h"

struct Decision {
	int audio, video;
	bool operator==(const Decision &) const = default;
};

int replay(const char *path)
{
	std::ifstream input(path);
	if (!input)
		return 2;
	std::string line;
	std::getline(input, line);
	webrtc::StreamSynchronization sync(2, 1);
	int rows = 0, abrupt_reductions = 0;
	while (std::getline(input, line)) {
		std::replace(line.begin(), line.end(), ',', ' ');
		std::istringstream row(line);
		int relative, audio, video, audio_floor, video_floor, adjusted, expected_audio, expected_video;
		if (!(row >> relative >> audio >> video >> audio_floor >> video_floor >> adjusted >> expected_audio >>
		      expected_video))
			return 2;
		sync.SetTargetBufferingDelay(std::min(audio_floor, video_floor));
		int target_audio = 0, target_video = video;
		const bool changed = sync.ComputeDelays(relative, audio, &target_audio, &target_video);
		if (changed != bool(adjusted) || target_audio != expected_audio || target_video != expected_video) {
			std::cerr << "Native synchronization replay diverged at row " << rows << '\n';
			return 1;
		}
		abrupt_reductions += changed && audio_floor > video_floor && video - target_video > 33;
		++rows;
	}
	std::cout << "replayed_decisions=" << rows << " abrupt_video_reductions=" << abrupt_reductions << '\n';
	// A passing characterization preserves the candidate's observed regression.
	return rows && abrupt_reductions ? 0 : 1;
}

std::vector<Decision> run(bool configured, int buffer, int audio_pipeline, int video_pipeline, bool jitter,
                          bool change_floor)
{
	webrtc::StreamSynchronization sync(2, 1);
	int audio = buffer, video = buffer, previous_floor = buffer;
	std::vector<Decision> result;
	for (int second = 0; second < 180; ++second) {
		// Shift both receiver floors and their realized delays together. The
		// relative synchronization problem is unchanged by the common floor.
		const int floor = buffer + (change_floor && second >= 60 && second < 120 ? 200 : 0);
		audio += floor - previous_floor;
		video += floor - previous_floor;
		previous_floor = floor;
		if (configured)
			sync.SetTargetBufferingDelay(floor);
		const int actual_audio = std::max(floor, audio) + audio_pipeline;
		int target_audio = 0, target_video = std::max(floor, video) + video_pipeline;
		if (sync.ComputeDelays(jitter ? (second % 2 ? 8 : -8) : 0, actual_audio, &target_audio, &target_video)) {
			audio = target_audio;
			video = target_video;
		}
		result.push_back({std::max(floor, audio) - floor, std::max(floor, video) - floor});
	}
	return result;
}

int main(int argc, char **argv)
{
	if (argc == 3 && std::string(argv[1]) == "replay")
		return replay(argv[2]);
	if (argc != 2 || (std::string(argv[1]) != "baseline" && std::string(argv[1]) != "configured"))
		return 2;
	const bool configured = std::string(argv[1]) == "configured";
	int mismatched = 0, scenarios = 0;
	std::cout << "policy,buffer,audio_pipeline,video_pipeline,jitter,reset,first_video_change_s,equivalent\n";
	for (int buffer : {0, 300, 1000})
		for (bool audio_later : {false, true})
			for (bool jitter : {false, true})
				for (int reset = 0; reset < 3; ++reset) {
					const int audio_pipeline = audio_later ? 70 : 0;
					const int video_pipeline = audio_later ? 0 : 70;
					const auto reference = run(true, 0, audio_pipeline, video_pipeline, jitter, reset == 2);
					const auto measured = run(configured, buffer, audio_pipeline, video_pipeline, jitter, reset == 2);
					const bool equivalent = reference == measured;
					int first = -1;
					for (size_t i = 0; i < measured.size(); ++i)
						if (measured[i].video != 0) {
							first = static_cast<int>(i);
							break;
						}
					std::cout << argv[1] << ',' << buffer << ',' << audio_pipeline << ',' << video_pipeline << ','
					          << jitter << ',' << reset << ',' << first << ',' << equivalent << '\n';
					mismatched += !equivalent;
					++scenarios;
				}
	std::cerr << "scenarios=" << scenarios << " mismatched=" << mismatched << '\n';
	// Baseline success means the configured-floor defect was reproduced.
	return configured ? (mismatched ? 1 : 0) : (mismatched ? 0 : 1);
}
