/**
 * @file ntp.hpp
 * @brief Main ntp library header
 * 
 * @license GPL-v3
 * @author GeorgyFirsov (gfirsov007@gmail.com)
 */

#pragma once

//
//            __
//    ____   / /_ ____
//   / __ \ / __// __ \                                                                                    
//  / / / // /_ / /_/ /
// /_/ /_/ \__// .___/
//            /_/
//     _   __        __   _                 ______ __                            __ ____                 __
//    / | / /____ _ / /_ (_)_   __ ___     /_  __// /_   _____ ___   ____ _ ____/ // __ \ ____   ____   / /
//   /  |/ // __ `// __// /| | / // _ \     / /  / __ \ / ___// _ \ / __ `// __  // /_/ // __ \ / __ \ / /
//  / /|  // /_/ // /_ / / | |/ //  __/    / /  / / / // /   /  __// /_/ // /_/ // ____// /_/ // /_/ // /
// /_/ |_/ \__,_/ \__//_/  |___/ \___/    /_/  /_/ /_//_/    \___/ \__,_/ \__,_//_/     \____/ \____//_/
//

//
// Just a list of necessary library headers
//
#include "ntp_config.hpp"
#include "details/time.hpp"
#include "logger/logger.hpp"
#include "pool/threadpool.hpp"
