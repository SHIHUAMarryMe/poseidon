// 这个文件是 Poseidon 服务器应用程序框架的一部分。
// Copyleft 2014 - 2017, LH_Mouse. All wrongs reserved.

#include "precompiled.hpp"
#include "optional_map.hpp"

namespace Poseidon {

OptionalMap::~OptionalMap(){ }

std::ostream &operator<<(std::ostream &os, const OptionalMap &rhs){
	os <<"{; ";
	for(AUTO(it, rhs.begin()); it != rhs.end(); ++it){
		os <<it->first <<" = (" <<it->second.size() <<")\"" <<it->second <<"\"; ";
	}
	os <<"}; ";
	return os;
}

}
