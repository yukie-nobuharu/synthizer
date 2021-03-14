#pragma once

#include "synthizer/biquad.hpp"
#include "synthizer/context.hpp"
#include "synthizer/effects/base_effect.hpp"
#include "synthizer/property_internals.hpp"
#include "synthizer/routable.hpp"
#include "synthizer/types.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <utility>

namespace synthizer {

/*
 * A global effect is an effect which sources route to on a one-by-one basis. To use this, apply the CRTP.
 * */
class GlobalEffect: public BaseEffect, public RouteInput {
	public:
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wuninitialized"
	GlobalEffect(const std::shared_ptr<Context> &ctx, unsigned int channels):
		input_buffer(),
		channels(channels),
		RouteInput(ctx, &this->input_buffer[0], channels) {}
#pragma clang diagnostic pop

	void run(unsigned int channels, float *destination) {
		syz_BiquadConfig biquad_cfg;

		if ((this->biquad_filter == nullptr || this->channels != last_channels) && this->channels != 0) {
			this->biquad_filter = createBiquadFilter(this->channels);
		}
		if (this->biquad_filter != nullptr) {
			if (this->acquireFilter(biquad_cfg)) {
				this->biquad_filter->configure(biquad_cfg);
			}
			this->biquad_filter->processBlock(&this->input_buffer[0], &this->input_buffer[0], false);
		}
		this->last_channels = this->channels;

		this->runEffect(this->time_in_blocks, this->channels, &this->input_buffer[0], channels, destination, (float)this->getGain());
		/*
		 * Reset this for the next time. This needs to live here since routers don't know about effects if
		 * there's no routing to them.
		 * */
		std::fill(&this->input_buffer[0], &this->input_buffer[this->channels * config::BLOCK_SIZE], 0.0f);
		this->time_in_blocks++;
	}

	#define PROPERTY_CLASS GlobalEffect
	#define PROPERTY_BASE BaseObject
	#define PROPERTY_LIST EFFECT_PROPERTIES
	#include "synthizer/property_impl.hpp"
	private:
	std::array<float, config::BLOCK_SIZE * config::MAX_CHANNELS> input_buffer = { 0.0f };
	unsigned int channels = 0;
	unsigned int time_in_blocks = 0;
	/* Filters. */
	unsigned int last_channels = 0;
	std::shared_ptr<BiquadFilter> biquad_filter = nullptr;
};

}
