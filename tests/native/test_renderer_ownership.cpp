#include "outlast_renderer_ownership.h"
#include <array>
#include <future>
#include <iostream>

using namespace olvr;
static unsigned checks=0;
static void require(bool value,const char* text) {
    ++checks;if (!value) throw std::runtime_error(text);
}
int main() {
    try {
        OutlastRendererOwnership owner,other;
        int viewport=0,family=0,capture_family=0,capture_renderer=0;
        std::array<int,3> renderers{};
        {
            OutlastRendererOwnership::FamilyScope capture(owner,&capture_family);
            require(!owner.constructed(&capture_renderer,&capture_family),
                "Enqueue outside the main viewport was tagged as gameplay");
        }
        {
            OutlastRendererOwnership::MainScope main(owner,&viewport);
            for (auto& renderer:renderers) {
                OutlastRendererOwnership::MainScope presentation(owner,&viewport,&renderer==&renderers[0]);
                OutlastRendererOwnership::FamilyScope enqueued(owner,&family);
                require(!owner.constructed(&capture_renderer,&capture_family),
                    "Unrelated nested constructor inherited the main family");
                require(owner.constructed(&renderer,&family),"Exact synchronous family constructor was not tagged");
            }
            {
                OutlastRendererOwnership::FamilyScope unrelated(other,&family);
                require(!other.constructed(&capture_renderer,&family),"Another controller inherited viewport ownership");
            }
        }
        require(owner.pending()==3,"Not all three native enqueue sites retained their independent renderers");
        for (auto& renderer:renderers) {
            const bool rendered=std::async(std::launch::async,[&] {
                OutlastRendererOwnership::DispatchScope dispatch(owner,&renderer);
                if (!dispatch.owned() || !owner.owns_render(&renderer) ||
                    owner.current_viewport(&renderer)!=&viewport || owner.owns_render(&capture_renderer) ||
                    owner.world_panel(&renderer)!=(&renderer==&renderers[0])) return false;
                {
                    OutlastRendererOwnership::DispatchScope capture(owner,&capture_renderer);
                    if (capture.owned() || owner.owns_render(&renderer) || owner.owns_render(&capture_renderer) ||
                        owner.world_panel(&capture_renderer))
                        return false;
                }
                return owner.owns_render(&renderer) && owner.world_panel(&renderer)==(&renderer==&renderers[0]);
            }).get();
            require(rendered,"Game-thread tagging did not transfer to the native render-thread scope");
            require(!owner.owns_render(&renderer),"Native view identity escaped its dispatcher lifetime");
        }
        require(owner.pending()==0,"Dispatcher destruction left stale renderer identities");
        {
            OutlastRendererOwnership::DispatchScope reused(owner,&renderers[0]);
            require(!reused.owned(),"A recycled native renderer inherited stale ownership");
        }
        {
            OutlastRendererOwnership::MainScope main(owner,&viewport);
            OutlastRendererOwnership::FamilyScope enqueued(owner,&family);
            require(owner.constructed(&renderers[0],&family),"Renderer address could not be reused after destruction");
            bool duplicate=false;
            try {owner.constructed(&renderers[0],&family);} catch (const std::logic_error&) {duplicate=true;}
            require(duplicate,"Duplicate construction was not refused");
            try {
                OutlastRendererOwnership::DispatchScope dispatch(owner,&renderers[0]);
                throw std::runtime_error("fixture unwind");
            } catch (const std::runtime_error&) {}
            require(!owner.owns_render(&renderers[0]) && owner.pending()==0,
                "Exception unwinding retained a destroyed renderer");
        }
        std::cout<<"PASS: main-family renderer ownership; independent_checks="<<checks
            <<"; exact families, three enqueue sites, render-thread transfer, capture exclusion, address reuse and native destruction lifetime\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;
    }
}
